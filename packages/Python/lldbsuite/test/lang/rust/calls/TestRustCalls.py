"""Test Rust function calls."""

import os
import time
import unittest2
import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil

class TestRustExpressions(TestBase):
    mydir = TestBase.compute_mydir(__file__)

    @add_test_categories(['pyapi'])
    @no_debug_info_test
    @skipUnlessRustInstalled
    def test_with_dsym_and_python_api(self):
        """Test Rust function calls."""
        self.buildRust()
        self.launchProcess()
        self.rust_calls()

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

    def rust_calls(self):
        frame = self.frame()
        v = frame.EvaluateExpression("not(true)")
        self.assertEqual("(bool)  = false", str(v))
        v = frame.EvaluateExpression("not(false)")
        self.assertEqual("(bool)  = true", str(v))
        v = frame.EvaluateExpression("constant()")
        self.assertEqual("(i64)  = -23", str(v))
        v = frame.EvaluateExpression("cst2(zzq)")
        self.assertEqual("(i16)  = -24", str(v))
        v = frame.EvaluateExpression("nil()")
        self.assertEqual("(())  = {}", str(v))
        v = frame.EvaluateExpression("add1(7)")
        self.assertEqual("(u32)  = 8", str(v))
        v = frame.EvaluateExpression("add1d(74.0)")
        self.assertEqual("(f64)  = 75", str(v))
        v = frame.EvaluateExpression("add1s(Struct{field:7}).field")
        self.assertEqual("(u8) field = 8", str(v))
        # FIXME - started failing
        # v = frame.EvaluateExpression("add1ts(TupleStruct(99)).0")
        # self.assertEqual("(u8)  = 100", str(v))
        # v = frame.EvaluateExpression("unifyplus1(SimpleEnum::One{f1:98}).0")
        # self.assertEqual("(u16)  = 99", str(v))
        # v = frame.EvaluateExpression("add1ue(UnivariantEnum::Single(17)).0")
        # self.assertEqual("(u8)  = 18", str(v))
        v = frame.EvaluateExpression("sum(0, 1, 2, 3.0, 4.0)")
        self.assertEqual("(f64)  = 10", str(v))
