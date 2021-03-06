#!/usr/bin/python

"""
MultiTestRunner - Harness for running multiple tests in the simple clang style.

TODO
--
 - Fix Ctrl-c issues
 - Use a timeout
 - Detect signalled failures (abort)
 - Better support for finding tests
"""

# TOD
import os, sys, re, random, time
import threading
import ProgressBar
import TestRunner
from TestRunner import TestStatus
from Queue import Queue

kTestFileExtensions = set(['.mi','.i','.c','.cpp','.m','.mm','.ll'])

kClangErrorRE = re.compile('(.*):([0-9]+):([0-9]+): error: (.*)')
kClangWarningRE = re.compile('(.*):([0-9]+):([0-9]+): warning: (.*)')
kAssertionRE = re.compile('Assertion failed: (.*, function .*, file .*, line [0-9]+\\.)')

def getTests(inputs):
    for path in inputs:
        if not os.path.exists(path):
            print >>sys.stderr,"WARNING: Invalid test \"%s\""%(path,)
            continue
        
        if os.path.isdir(path):
            for dirpath,dirnames,filenames in os.walk(path):
                dotTests = os.path.join(dirpath,'.tests')
                if os.path.exists(dotTests):
                    for ln in open(dotTests):
                        if ln.strip():
                            yield os.path.join(dirpath,ln.strip())
                else:
                    # FIXME: This doesn't belong here
                    if 'Output' in dirnames:
                        dirnames.remove('Output')
                    for f in filenames:
                        base,ext = os.path.splitext(f)
                        if ext in kTestFileExtensions:
                            yield os.path.join(dirpath,f)
        else:
            yield path

class TestingProgressDisplay:
    def __init__(self, opts, numTests, progressBar=None):
        self.opts = opts
        self.numTests = numTests
        self.digits = len(str(self.numTests))
        self.current = None
        self.lock = threading.Lock()
        self.progressBar = progressBar
        self.progress = 0.

    def update(self, index, tr):
        # Avoid locking overhead in quiet mode
        if self.opts.quiet and not tr.failed():
            return

        # Output lock
        self.lock.acquire()
        try:
            self.handleUpdate(index, tr)
        finally:
            self.lock.release()

    def finish(self):
        if self.progressBar:
            self.progressBar.clear()
        elif self.opts.succinct:
            sys.stdout.write('\n')

    def handleUpdate(self, index, tr):
        if self.progressBar:
            if tr.failed():
                self.progressBar.clear()
            else:
                # Force monotonicity
                self.progress = max(self.progress, float(index)/self.numTests)
                self.progressBar.update(self.progress, tr.path)
                return
        elif self.opts.succinct:
            if not tr.failed():
                sys.stdout.write('.')
                sys.stdout.flush()
                return
            else:
                sys.stdout.write('\n')

        extra = ''
        if tr.code==TestStatus.Invalid:
            extra = ' - (Invalid test)'
        elif tr.code==TestStatus.NoRunLine:
            extra = ' - (No RUN line)'
        elif tr.failed():
            extra = ' - %s'%(TestStatus.getName(tr.code).upper(),)
        print '%*d/%*d - %s%s'%(self.digits, index+1, self.digits, 
                              self.numTests, tr.path, extra)

        if tr.failed():
            msgs = []
            if tr.warnings:
                msgs.append('%d warnings'%(len(tr.warnings),))
            if tr.errors:
                msgs.append('%d errors'%(len(tr.errors),))
            if tr.assertions:
                msgs.append('%d assertions'%(len(tr.assertions),))
            
            if msgs:
                print '\tFAIL (%s)'%(', '.join(msgs))
            for i,error in enumerate(set([e for (_,_,_,e) in tr.errors])):
                print '\t\tERROR: %s'%(error,)
                if i>20:
                    print '\t\t\t(too many errors, skipping)'
                    break
            for assertion in set(tr.assertions):
                print '\t\tASSERTION: %s'%(assertion,)
            if self.opts.showOutput:
                TestRunner.cat(tr.testResults, sys.stdout)

class TestResult:
    def __init__(self, path, code, testResults):
        self.path = path
        self.code = code
        self.testResults = testResults
        self.warnings = []
        self.errors = []
        self.assertions = []

        if self.failed():
            f = open(self.testResults)
            data = f.read()
            f.close()
            self.warnings = [m.groups() for m in kClangWarningRE.finditer(data)]
            self.errors = [m.groups() for m in kClangErrorRE.finditer(data)]
            self.assertions = [m.group(1) for m in kAssertionRE.finditer(data)]

    def failed(self):
        return self.code in (TestStatus.Fail,TestStatus.XPass)
        
class TestProvider:
    def __init__(self, opts, tests, display):
        self.opts = opts
        self.tests = tests
        self.index = 0
        self.lock = threading.Lock()
        self.results = [None]*len(self.tests)
        self.startTime = time.time()
        self.progress = display

    def get(self):
        self.lock.acquire()
        try:
            if self.opts.maxTime is not None:
                if time.time() - self.startTime > self.opts.maxTime:
                    return None
            if self.index >= len(self.tests):
                return None
            item = self.tests[self.index],self.index
            self.index += 1
            return item
        finally:
            self.lock.release()

    def setResult(self, index, result):
        self.results[index] = result
        self.progress.update(index, result)
    
class Tester(threading.Thread):
    def __init__(self, provider):
        threading.Thread.__init__(self)
        self.provider = provider
    
    def run(self):
        while 1:
            item = self.provider.get()
            if item is None:
                break
            self.runTest(item)

    def runTest(self, (path,index)):
        command = path
        # Use hand concatentation here because we want to override
        # absolute paths.
        output = 'Output/' + path + '.out'
        testname = path
        testresults = 'Output/' + path + '.testresults'
        TestRunner.mkdir_p(os.path.dirname(testresults))
        numTests = len(self.provider.tests)
        digits = len(str(numTests))
        code = None
        try:
            opts = self.provider.opts
            if opts.debugDoNotTest:
                code = None
            else:
                code = TestRunner.runOneTest(path, command, output, testname, 
                                             opts.clang,
                                             useValgrind=opts.useValgrind,
                                             useDGCompat=opts.useDGCompat,
                                             useScript=opts.testScript,
                                             output=open(testresults,'w'))
        except KeyboardInterrupt:
            # This is a sad hack. Unfortunately subprocess goes
            # bonkers with ctrl-c and we start forking merrily.
            print 'Ctrl-C detected, goodbye.'
            os.kill(0,9)

        self.provider.setResult(index, TestResult(path, code, testresults))

def detectCPUs():
    """
    Detects the number of CPUs on a system. Cribbed from pp.
    """
    # Linux, Unix and MacOS:
    if hasattr(os, "sysconf"):
        if os.sysconf_names.has_key("SC_NPROCESSORS_ONLN"):
            # Linux & Unix:
            ncpus = os.sysconf("SC_NPROCESSORS_ONLN")
            if isinstance(ncpus, int) and ncpus > 0:
                return ncpus
        else: # OSX:
            return int(os.popen2("sysctl -n hw.ncpu")[1].read())
    # Windows:
    if os.environ.has_key("NUMBER_OF_PROCESSORS"):
        ncpus = int(os.environ["NUMBER_OF_PROCESSORS"]);
        if ncpus > 0:
            return ncpus
        return 1 # Default

def main():
    global options
    from optparse import OptionParser
    parser = OptionParser("usage: %prog [options] {inputs}")
    parser.add_option("-j", "--threads", dest="numThreads",
                      help="Number of testing threads",
                      type=int, action="store", 
                      default=detectCPUs())
    parser.add_option("", "--clang", dest="clang",
                      help="Program to use as \"clang\"",
                      action="store", default="clang")
    parser.add_option("", "--vg", dest="useValgrind",
                      help="Run tests under valgrind",
                      action="store_true", default=False)
    parser.add_option("", "--dg", dest="useDGCompat",
                      help="Use llvm dejagnu compatibility mode",
                      action="store_true", default=False)
    parser.add_option("", "--script", dest="testScript",
                      help="Default script to use",
                      action="store", default=None)
    parser.add_option("-v", "--verbose", dest="showOutput",
                      help="Show all test output",
                      action="store_true", default=False)
    parser.add_option("-q", "--quiet", dest="quiet",
                      help="Suppress no error output",
                      action="store_true", default=False)
    parser.add_option("-s", "--succinct", dest="succinct",
                      help="Reduce amount of output",
                      action="store_true", default=False)
    parser.add_option("", "--max-tests", dest="maxTests",
                      help="Maximum number of tests to run",
                      action="store", type=int, default=None)
    parser.add_option("", "--max-time", dest="maxTime",
                      help="Maximum time to spend testing (in seconds)",
                      action="store", type=float, default=None)
    parser.add_option("", "--shuffle", dest="shuffle",
                      help="Run tests in random order",
                      action="store_true", default=False)
    parser.add_option("", "--seed", dest="seed",
                      help="Seed for random number generator (default: random).",
                      action="store", default=None)
    parser.add_option("", "--no-progress-bar", dest="useProgressBar",
                      help="Do not use curses based progress bar",
                      action="store_false", default=True)
    parser.add_option("", "--debug-do-not-test", dest="debugDoNotTest",
                      help="DEBUG: Skip running actual test script",
                      action="store_true", default=False)
    (opts, args) = parser.parse_args()

    if not args:
        parser.error('No inputs specified')

    allTests = list(getTests(args))
    allTests.sort()
    
    tests = allTests
    if opts.seed is not None:
        try:
            seed = int(opts.seed)
        except:
            parser.error('--seed argument should be an integer')
        random.seed(seed)
    if opts.shuffle:
        random.shuffle(tests)
    if opts.maxTests is not None:
        tests = tests[:opts.maxTests]

    extra = ''
    if len(tests) != len(allTests):
        extra = ' of %d'%(len(allTests),)
    header = '-- Testing: %d%s tests, %d threads --'%(len(tests),extra,opts.numThreads)

    progressBar = None
    if not opts.quiet:
        if opts.useProgressBar:
            try:
                tc = ProgressBar.TerminalController()
                progressBar = ProgressBar.ProgressBar(tc, header)
            except ValueError:
                pass

        if not progressBar:
            print header

    display = TestingProgressDisplay(opts, len(tests), progressBar)
    provider = TestProvider(opts, tests, display)

    testers = [Tester(provider) for i in range(opts.numThreads)]
    startTime = time.time()
    for t in testers:
        t.start()
    try:
        for t in testers:
            t.join()
    except KeyboardInterrupt:
        sys.exit(1)

    display.finish()

    if not opts.quiet:
        print 'Testing Time: %.2fs'%(time.time() - startTime)

    xfails = [i for i in provider.results if i and i.code==TestStatus.XFail]
    if xfails:
        print '*'*20
        print 'Expected Failures (%d):' % len(xfails)
        for tr in xfails:
            print '\t%s'%(tr.path,)

    xpasses = [i for i in provider.results if i and i.code==TestStatus.XPass]
    if xpasses:
        print '*'*20
        print 'Unexpected Passing Tests (%d):' % len(xpasses)
        for tr in xpasses:
            print '\t%s'%(tr.path,)

    failures = [i for i in provider.results if i and i.code==TestStatus.Fail]
    if failures:
        print '*'*20
        print 'Failing Tests (%d):' % len(failures)
        for tr in failures:
            if tr.code != TestStatus.XPass:
                print '\t%s'%(tr.path,)

        print '\nFailures: %d'%(len(failures),)

        assertions = {}
        errors = {}
        errorFree = []
        for tr in failures:
            if not tr.errors and not tr.assertions:
                errorFree.append(tr)
            for (_,_,_,error) in tr.errors:
                errors[error] = errors.get(error,0) + 1
            for assertion in tr.assertions:
                assertions[assertion] = assertions.get(assertion,0) + 1
        if errorFree:
            print 'Failures w/o Errors (%d):' % len(errorFree)
            for tr in errorFree:
                print '\t%s'%(tr.path,)
        
        if errors:
            print 'Error Summary (%d):' % sum(errors.values())
            items = errors.items()
            items.sort(key = lambda (_,v): -v)
            for i,(error,count) in enumerate(items):
                print '\t%3d: %s'%(count,error)
                if i>100:
                    print '\t\t(too many errors, skipping)'
                    break

        if assertions:
            print 'Assertion Summary (%d):' % sum(assertions.values())
            items = assertions.items()
            items.sort(key = lambda (_,v): -v)
            for assertion,count in items:
                print '\t%3d: %s'%(count,assertion)

if __name__=='__main__':
    main()
