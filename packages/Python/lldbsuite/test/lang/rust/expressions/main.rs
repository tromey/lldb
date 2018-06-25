#![allow(dead_code)]
#![allow(unused_variables)]
#![allow(unused_assignments)]

fn do_nothing() { }

pub struct Struct {
    field1: u8,
    field2: char,
}

struct TupleStruct(u8, char);

pub union Union {
    field1: u8,
    field2: char,
}

pub enum CLikeEnum {
    MinusOne = -1,
    Zero,
    One,
}

pub enum SimpleEnum {
    One{f1: u8, f2: u8},
    Two(u16, u16)
}

pub enum OptimizedEnum {
    Null,
    NonNull(Box<u8>)
}

fn main() {
    let vbool: bool = true;

    let mut vchar: char = 'Q';

    let vi8: i8 = -23;
    let mut vu8: u8 = 23;
    let vi16: i16 = -2323;
    let vu16: u16 = 2323;
    let vi32: i32 = -232323;
    let vu32: u32 = 232323;
    let vi64: i64 = -23232323;
    let vu64: u64 = 23232323;

    let visize: isize = -23232323;
    let vusize: usize = 23232323;

    let vf32: f32 = 5.25;
    let vf64: f64 = 7.5;

    let vi8array : [i8; 4] = [1,2,3,4];

    let empty = ();

    let vstruct = Struct { field1: 23, field2: 'Q' };
    let vtuplestruct = TupleStruct(23, 'Q');
    let vtuple = (23u8, 'Q');
    let vunion = Union { field2: 'Q' };

    let vboolpointer = &vbool as *const bool;
    let vcharpointer = &mut vchar as *mut char;
    let vi8ref = &vi8;
    let vu8ref = &mut vu8;

    let vclikeenum = CLikeEnum::MinusOne;

    let vsimpleenum = SimpleEnum::Two(83, 92);
    let vsimpleenum1 = SimpleEnum::One{f1: 92, f2: 83};
    let voptenum = OptimizedEnum::Null;

    let vbstr = b"bytes";
    let vstr = "string";

    let vsimpleenum_ref = &vsimpleenum;

    let vrange = 0..5;
    let vrange_from = 0..;
    let vrange_to = ..5;
    let vrange_full = ..;

    do_nothing();               // breakpoint
}
