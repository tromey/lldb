"""Test the Rust expression parser and evaluator."""

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
        """Test Rust expression parser and evaluator."""
        self.buildRust()
        self.launchProcess()
        self.rust_expressions()

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

    def rust_expressions(self):
        frame = self.frame()
        v = frame.EvaluateExpression("1")
        self.assertEqual("(i32)  = 1", str(v))
        v = frame.EvaluateExpression("1.25")
        self.assertEqual("(f64)  = 1.25", str(v))
        v = frame.EvaluateExpression("1 + 2")
        self.assertEqual("(i32)  = 3", str(v))
        v = frame.EvaluateExpression("+ 2 + - 1")
        self.assertEqual("(i32)  = 1", str(v))
        v = frame.EvaluateExpression("3 + 1 / 4")
        self.assertEqual("(i32)  = 3", str(v))
        v = frame.EvaluateExpression("(3 + 1) / 4")
        self.assertEqual("(i32)  = 1", str(v))
        v = frame.EvaluateExpression("5 % 4")
        self.assertEqual("(i32)  = 1", str(v))
        v = frame.EvaluateExpression("sizeof(4u8)")
        self.assertEqual("(usize)  = 1", str(v))
        v = frame.EvaluateExpression("!0xffu16")
        self.assertEqual("(u16)  = 65280", str(v))
        v = frame.EvaluateExpression("1 << 3")
        self.assertEqual("(i32)  = 8", str(v))
        v = frame.EvaluateExpression("-1 < 3")
        self.assertEqual("(bool)  = true", str(v))
        v = frame.EvaluateExpression("3 <= 3")
        self.assertEqual("(bool)  = true", str(v))
        v = frame.EvaluateExpression("3 >= 3")
        self.assertEqual("(bool)  = true", str(v))
        v = frame.EvaluateExpression("-9 > -11")
        self.assertEqual("(bool)  = true", str(v))
        v = frame.EvaluateExpression("-27 != -17")
        self.assertEqual("(bool)  = true", str(v))
        v = frame.EvaluateExpression("-27 == -17")
        self.assertEqual("(bool)  = false", str(v))
        v = frame.EvaluateExpression("5.0 / 4")
        self.assertEqual("(f64)  = 1.25", str(v))
        v = frame.EvaluateExpression("'c'")
        self.assertEqual("(char)  = 'c'", str(v))
        v = frame.EvaluateExpression("true")
        self.assertEqual("(bool)  = true", str(v))
        v = frame.EvaluateExpression("false")
        self.assertEqual("(bool)  = false", str(v))
        v = frame.EvaluateExpression("!true")
        self.assertEqual("(bool)  = false", str(v))
        v = frame.EvaluateExpression("vstruct.field1")
        self.assertEqual("(u8) field1 = 23", str(v))
        v = frame.EvaluateExpression("vtuplestruct.0")
        self.assertEqual("(u8) 0 = 23", str(v))
        v = frame.EvaluateExpression("vtuple.0")
        self.assertEqual("(u8) 0 = 23", str(v))
        v = frame.EvaluateExpression("vunion.field2")
        self.assertEqual("(char) field2 = 'Q'", str(v))
        v = frame.EvaluateExpression("vi8array[2]")
        self.assertEqual("(i8) [2] = 3", str(v))
        v = frame.EvaluateExpression("*vboolpointer")
        self.assertEqual("(bool) *vboolpointer = true", str(v))
        v = frame.EvaluateExpression("*vcharpointer")
        self.assertEqual("(char) *vcharpointer = 'Q'", str(v))
        v = frame.EvaluateExpression("*vi8ref")
        self.assertEqual("(i8) *vi8ref = -23", str(v))
        v = frame.EvaluateExpression("*&vi8")
        self.assertEqual("(i8) *&vi8 = -23", str(v))
        v = frame.EvaluateExpression("*vu8ref")
        self.assertEqual("(u8) *vu8ref = 23", str(v))
        v = frame.EvaluateExpression("vsimpleenum", lldb.eDynamicDontRunTarget)
        self.assertEqual("(main::SimpleEnum::Two) vsimpleenum = (0 = 83, 1 = 92)", str(v))
        v = frame.EvaluateExpression("vsimpleenum.1")
        self.assertEqual("(u16) 1 = 92", str(v))
        v = frame.EvaluateExpression("vsimpleenum1.f2")
        self.assertEqual("(u8) f2 = 83", str(v))
        v = frame.EvaluateExpression("vi8 = 7")
        self.assertEqual("(i8) vi8 = 7", str(v))
        # Double check.
        v = frame.EvaluateExpression("*&vi8")
        self.assertEqual("(i8) *&vi8 = 7", str(v))
        v = frame.EvaluateExpression("vi8 += 7")
        self.assertEqual("(i8) vi8 = 14", str(v))
        # Double check.
        v = frame.EvaluateExpression("*&vi8")
        self.assertEqual("(i8) *&vi8 = 14", str(v))
        v = frame.EvaluateExpression("[23i64; 5]")
        self.assertEqual("([i64; 5])  = ([0] = 23, [1] = 23, [2] = 23, [3] = 23, [4] = 23)", str(v))
        v = frame.EvaluateExpression("23 as u8")
        self.assertEqual("(u8)  = 23", str(v))
        v = frame.EvaluateExpression('b"hi"')
        self.assertEqual("([u8; 2]) * = ([0] = 104, [1] = 105)", str(v))
        # FIXME need pretty-printing for &str
        # v = frame.EvaluateExpression('"hi"')
        # self.assertEqual("fixme", str(v))
        v = frame.EvaluateExpression("Struct { field1: 8, field2: 'c'}")
        self.assertEqual("(main::Struct) * = (field1 = 8, field2 = 'c')", str(v))
        v = frame.EvaluateExpression("Struct { field1: 8, .. vstruct}")
        self.assertEqual("(main::Struct) * = (field1 = 8, field2 = 'Q')", str(v))
        v = frame.EvaluateExpression("TupleStruct(24, 'R')")
        self.assertEqual("(main::TupleStruct) * = (0 = 24, 1 = 'R')", str(v))
        v = frame.EvaluateExpression("0..5")
        self.assertEqual("(core::ops::range::Range<i32>) * = (start = 0, end = 5)", str(v))
        # v = frame.EvaluateExpression("0..=5")
        # self.assertEqual("(core::ops::range::RangeInclusive<i32>) * = (start = 0, end = 5)", str(v))
        v = frame.EvaluateExpression("..5")
        self.assertEqual("(core::ops::range::RangeTo<i32>) * = (end = 5)", str(v))
        # v = frame.EvaluateExpression("..=5")
        # self.assertEqual("(core::ops::range::RangeToInclusive<i32> * = (end = 5)", str(v))
        v = frame.EvaluateExpression("0..")
        self.assertEqual("(core::ops::range::RangeFrom<i32>) * = (start = 0)", str(v))
        # Can't allocate a zero-length object
        # v = frame.EvaluateExpression("..")
        # self.assertEqual("(core::ops::range::RangeFull) * = ()", str(v))
