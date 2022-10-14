# This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details

import argparse
import os.path
import subprocess as sp
import sys
import xml.sax as x

SCRIPT_PATH = os.path.split(sys.argv[0])[0]
FAIL_LIST_PATH = os.path.join(SCRIPT_PATH, "faillist.txt")


def loadFailList():
    with open(FAIL_LIST_PATH) as f:
        return set(map(str.strip, f.readlines()))


def safeParseInt(i, default=0):
    try:
        return int(i)
    except ValueError:
        return default


def makeDottedName(path):
    return ".".join(path)


class Handler(x.ContentHandler):
    def __init__(self, failList):
        self.currentTest = []
        self.failList = failList  # Set of dotted test names that are expected to fail

        self.results = {}  # {DottedName: TrueIfTheTestPassed}

        self.numSkippedTests = 0

    def startElement(self, name, attrs):
        if name == "TestSuite":
            self.currentTest.append(attrs["name"])
        elif name == "TestCase":
            self.currentTest.append(attrs["name"])

        elif name == "OverallResultsAsserts":
            if self.currentTest:
                passed = attrs["test_case_success"] == "true"

                dottedName = makeDottedName(self.currentTest)

                # Sometimes we get multiple XML trees for the same test. All of
                # them must report a pass in order for us to consider the test
                # to have passed.
                r = self.results.get(dottedName, True)
                self.results[dottedName] = r and passed

        elif name == "OverallResultsTestCases":
            self.numSkippedTests = safeParseInt(attrs.get("skipped", 0))

    def endElement(self, name):
        if name == "TestCase":
            self.currentTest.pop()

        elif name == "TestSuite":
            self.currentTest.pop()


def print_stderr(*args, **kw):
    print(*args, **kw, file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(
        description="Run Luau.UnitTest with deferred constraint resolution enabled"
    )
    parser.add_argument(
        "path", action="store", help="Path to the Luau.UnitTest executable"
    )
    parser.add_argument(
        "--dump",
        dest="dump",
        action="store_true",
        help="Instead of doing any processing, dump the raw output of the test run.  Useful for debugging this tool.",
    )
    parser.add_argument(
        "--write",
        dest="write",
        action="store_true",
        help="Write a new faillist.txt after running tests.",
    )

    parser.add_argument("--randomize", action="store_true", help="Pick a random seed")

    parser.add_argument(
        "--random-seed",
        action="store",
        dest="random_seed",
        type=int,
        help="Accept a specific RNG seed",
    )

    args = parser.parse_args()

    failList = loadFailList()

    commandLine = [
        args.path,
        "--reporters=xml",
        "--fflags=true,DebugLuauDeferredConstraintResolution=true",
    ]

    if args.random_seed:
        commandLine.append("--random-seed=" + str(args.random_seed))
    elif args.randomize:
        commandLine.append("--randomize")

    print_stderr(">", " ".join(commandLine))

    p = sp.Popen(
        commandLine,
        stdout=sp.PIPE,
    )

    handler = Handler(failList)

    if args.dump:
        for line in p.stdout:
            sys.stdout.buffer.write(line)
        return
    else:
        try:
            x.parse(p.stdout, handler)
        except x.SAXParseException as e:
            print_stderr(
                f"XML parsing failed during test {makeDottedName(handler.currentTest)}.  That probably means that the test crashed"
            )
            sys.exit(1)

    p.wait()

    for testName, passed in handler.results.items():
        if passed and testName in failList:
            print_stderr(f"UNEXPECTED: {testName} should have failed")
        elif not passed and testName not in failList:
            print_stderr(f"UNEXPECTED: {testName} should have passed")

    if args.write:
        newFailList = sorted(
            (
                dottedName
                for dottedName, passed in handler.results.items()
                if not passed
            ),
            key=str.lower,
        )
        with open(FAIL_LIST_PATH, "w", newline="\n") as f:
            for name in newFailList:
                print(name, file=f)
        print_stderr("Updated faillist.txt")

    if handler.numSkippedTests > 0:
        print_stderr(
            f"{handler.numSkippedTests} test(s) were skipped!  That probably means that a test segfaulted!"
        )
        sys.exit(1)

    ok = all(
        not passed == (dottedName in failList)
        for dottedName, passed in handler.results.items()
    )

    if ok:
        print_stderr("Everything in order!")

    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()