"""Test DWARF type parsing for Rust."""

from __future__ import print_function

import lldb
import os

from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil


class TestRustASTContext(TestBase):
    mydir = TestBase.compute_mydir(__file__)

    @add_test_categories(['pyapi'])
    @no_debug_info_test
    @skipUnlessRustInstalled
    def test_with_dsym_and_python_api(self):
        """Test RustASTContext DWARF parsing."""
        self.buildRust()
        self.launchProcess()
        self.init_typelist()
        self.check_types()
        self.check_main_vars()
        self.check_main_function()
        self.check_structs()
        self.check_enums()
        self.check_generics()

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

    def init_typelist(self):
        address_size = self.target().GetAddressByteSize()
        self._typelist = []
        for (name, size, value) in [
                ('bool', 1, 'true'),
                ('char', 4, "'Q'"),
                ('i8', 1, '-23'),
                ('u8', 1, '23'),
                ('i16', 2, '-2323'),
                ('u16', 2, '2323'),
                ('i32', 4, '-232323'),
                ('u32', 4, '232323'),
                ('i64', 8, '-23232323'),
                ('u64', 8, '23232323'),
                ('isize', address_size, '-23232323'),
                ('usize', address_size, '23232323'),
                ('f32', 4, '5.25'),
                ('f64', 8, '7.5'),
        ]:
            self._typelist.append((name, 'v' + name, size, value))

    def check_type(self, name, size, typeclass):
        tl = self.target().FindTypes(name)
        self.assertTrue(len(tl) > 0)
        t = list(tl)[0]
        self.assertEqual(name, t.name)
        self.assertEqual(typeclass, t.type)
        self.assertEqual(size, t.size)

    def check_types(self):
        for (name, vname, size, value) in self._typelist:
            self.check_type(name, size, lldb.eTypeClassBuiltin)

    def var(self, name):
        var = self.frame().FindVariable(name)
        self.assertTrue(var.IsValid(), "%s %s" % (VALID_VARIABLE, name))
        return var

    def check_main_vars(self):
        mytypelist = self._typelist[:]
        # Not in _typelist because it isn't eTypeClassBuiltin; and
        # note that the output of "{}" can't be changed.
        mytypelist.append(('()', 'empty', 0, '{}'))
        # Note there doesn't seem to be a way to customize the array
        # formatting to be more rust-like.
        mytypelist.append(('[i8; 4]', 'vi8array', 4, '([0] = 1, [1] = 2, [2] = 3, [3] = 4)'))
        address_size = self.target().GetAddressByteSize()
        mytypelist.append(('*const bool', 'vboolpointer', address_size, None))
        mytypelist.append(('*mut char', 'vcharpointer', address_size, None))
        mytypelist.append(('&i8', 'vi8ref', address_size, None))
        mytypelist.append(('&mut u8', 'vu8ref', address_size, None))
        mytypelist.append(('main::CLikeEnum', 'vclikeenum', 1, 'main::CLikeEnum::MinusOne'))

        for (name, vname, size, value) in mytypelist:
            v = self.var(vname)
            self.assertEqual(name, v.GetType().name)
            self.assertEqual(size, v.GetType().GetByteSize())
            # Some values can't really be checked.
            if value is not None:
                expected = "(" + name + ") " + vname + " = " + value
                self.assertEqual(expected, str(v))
            # Handy for debugging.
            # else:
            #     print("GOT === " + str(v))

    def check_main_function(self):
        fn_type = self.frame().GetFunction().GetType()
        self.assertTrue(fn_type.IsFunctionType())
        self.assertEqual(len(fn_type.GetFunctionArgumentTypes()), 0)
        self.assertEqual(fn_type.GetFunctionReturnType().name, '()')

    def check_structs(self):
        for (vname, typename, m0name, m1name, desc) in [
                ('vstruct', 'main::Struct', 'field1', 'field2',
                 'struct main::Struct {\n  field1: u8,\n  field2: char\n}'),
                ('vtuplestruct', 'main::TupleStruct', '0', '1',
                 'struct main::TupleStruct (\n  0: u8,\n  1: char\n)'),
                ('vtuple', '(u8, char)', '0', '1',
                 '(\n  0: u8,\n  1: char\n)'),
                ('vunion', 'main::Union', 'field1', 'field2',
                 'union main::Union {\n  field1: u8,\n  field2: char\n}'),
        ]:
            v = self.var(vname)
            vtype = v.GetType()
            self.assertEqual(str(vtype), desc)
            self.assertEqual(typename, vtype.name)
            self.assertTrue(vtype.IsTypeComplete())
            self.assertEqual(vtype.GetNumberOfFields(), 2)
            m0 = vtype.GetFieldAtIndex(0)
            self.assertEqual(m0.GetType().name, 'u8')
            self.assertEqual(m0.GetName(), m0name)
            m1 = vtype.GetFieldAtIndex(1)
            self.assertEqual(m1.GetType().name, 'char')
            self.assertEqual(m1.GetName(), m1name)

    def check_enums(self):
        address_size = self.target().GetAddressByteSize()
        mytypelist = []
        mytypelist.append(('main::SimpleEnum::Two', 'vsimpleenum', 6, '(0 = 83, 1 = 92)'))
        mytypelist.append(('main::OptimizedEnum::Null', 'voptenum', address_size, '{}'))
        mytypelist.append(('main::OptimizedEnum::NonNull', 'voptenum2', address_size, None))
        for (name, vname, size, value) in mytypelist:
            v = self.var(vname).dynamic
            # See https://github.com/rust-lang-nursery/lldb/issues/24
            # self.assertEqual(name, v.GetType().name)
            self.assertEqual(size, v.GetType().GetByteSize())
            self.assertEqual(0, v.GetType().num_template_args)
            # Some values can't really be checked.
            if value is not None:
                expected = "(" + name + ") " + vname + " = " + value
                self.assertEqual(expected, str(v.dynamic))

    def check_generics(self):
        t = self.var('vgeneric').GetType()
        self.assertEqual(1, t.num_template_args)
        self.assertEqual('T', t.template_args[0].name)
        self.assertEqual('i32', t.template_args[0].GetTypedefedType().name)
        t = self.frame().EvaluateExpression("generic_function<i32>").GetType().GetPointeeType()
        self.assertEqual(1, t.num_template_args)
        self.assertEqual('T', t.template_args[0].name)
        self.assertEqual('i32', t.template_args[0].GetTypedefedType().name)
