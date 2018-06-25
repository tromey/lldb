"""Test name lookup for Rust."""

from __future__ import print_function

import lldb
import os

from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil


class TestRustNames(TestBase):
    mydir = TestBase.compute_mydir(__file__)

    @add_test_categories(['pyapi'])
    @no_debug_info_test
    @skipUnlessRustInstalled
    def test_with_dsym_and_python_api(self):
        """Test Rust name lookup."""
        self.buildRust()
        self.launchProcess()
        self.check_names()

    def setUp(self):
        # Call super's setUp().
        TestBase.setUp(self)
        # Find the line numbers to break inside main().
        self.main_source = "main.rs"
        self.break_line = line_number(self.main_source, '// breakpoint')

    def launchProcess(self):
        exe = os.path.join(os.getcwd(), "main")

        target = self.dbg.CreateTarget(exe)
        self.assertTrue(target, VALID_TARGET)

        bpt = target.BreakpointCreateByLocation(self.main_source, self.break_line)
        self.assertTrue(bpt, VALID_BREAKPOINT)

        # Now launch the process, and do not stop at entry point.
        process = target.LaunchSimple(None, None, self.get_process_working_directory())

        self.assertTrue(process, PROCESS_IS_VALID)

        # The stop reason of the thread should be breakpoint.
        thread_list = lldbutil.get_threads_stopped_at_breakpoint(process, bpt)

        # Make sure we stopped at the first breakpoint.
        self.assertTrue(
            len(thread_list) != 0,
            "No thread stopped at our breakpoint.")
        self.assertTrue(len(thread_list) == 1,
                        "More than one thread stopped at our breakpoint.")

        frame = thread_list[0].GetFrameAtIndex(0)
        self.assertTrue(frame, "Got a valid frame 0 frame.")

    def check_names(self):
        frame = self.frame()
        namelist = [
            ('::VALUE', 7777, 'v0'),
            ('::m1::VALUE', 1, 'v1'),
            ('::m1::m1_1::VALUE', 11, 'v1_1'),
            ('::m1::m1_2::VALUE', 12, 'v1_2'),
            ('::m2::VALUE', 2, 'v2'),
            ('::m2::m2_1::VALUE', 21, 'v2_1'),
            ('::m2::m2_1::m2_1_2::VALUE', 212, 'v2_1_2'),
            ('::m2::m2_2::VALUE', 22, 'v22'),

            ('VALUE', 212, 'v'),
            ('self::VALUE', 212, 'svalue'),
            ('super::VALUE', 21, 'suvalue'),
            ('self::super::VALUE', 21, 'ssuvalue'),
            ('super::super::VALUE', 2, 'susuvalue'),
            ('self::super::super::VALUE', 2, 'ssusuvalue'),
            ('super::super::super::VALUE', 7777, 'sususuvalue'),
            ('self::super::super::super::VALUE', 7777, 'ssususuvalue'),

            ('m2_1_2_1::VALUE', 2121, 'rv'),
            ('self::m2_1_2_1::VALUE', 2121, 'srv'),
            ('super::m2_1_2::m2_1_2_1::VALUE', 2121, 'srv'),
        ]
        for (name, value, local) in namelist:
            c = frame.EvaluateExpression(local)
            self.assertEqual(c.GetValueAsSigned(), value, 'checking compiler ' + name)
            v = frame.EvaluateExpression(name)
            self.assertEqual(v.GetValueAsSigned(), value, 'checking ' + name)
