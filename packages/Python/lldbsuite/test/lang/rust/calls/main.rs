#![allow(dead_code)]
#![allow(unused_variables)]
#![allow(unused_assignments)]

fn do_nothing() { }

pub fn not(x: bool) -> bool {
    !x
}

pub fn constant() -> i64 {
    -23
}

pub fn cst2(x: ()) -> i16 {
    -24
}

pub fn nil() -> () {
    ()
}

pub fn add1(x: u32) -> u32 {
    x + 1
}

pub fn add1d(x: f64) -> f64 {
    x + 1f64
}

pub struct Struct {
    field: u8
}

pub fn add1s(x: Struct) -> Struct {
    Struct{field: x.field + 1}
}

pub struct TupleStruct(u8);

pub fn add1ts(x: TupleStruct) -> TupleStruct {
    TupleStruct(x.0 + 1)
}

pub enum SimpleEnum {
    One{f1: u16},
    Two(u16)
}

pub fn unifyplus1(x: SimpleEnum) -> SimpleEnum {
    match x {
        SimpleEnum::One{f1: v} => SimpleEnum::Two(v + 1),
        SimpleEnum::Two(v) => SimpleEnum::Two(v + 1)
    }
}

pub enum UnivariantEnum {
    Single(u8)
}

pub fn add1ue(x: UnivariantEnum) -> u8 {
    match x {
        UnivariantEnum::Single(v) => v + 1
    }
}

pub fn sum(a: u8, b: u16, c: u32, d: f32, e: f64) -> f64 {
    a as f64 + b as f64 + c as f64 + d as f64 + e
}

fn main() {
    let a = not(false);
    let b = constant();
    let zzq = ();
    let c = cst2(zzq);
    let d = nil();
    let e = add1(7);
    let f = add1d(7.0);
    let g = add1s(Struct{field:8});
    let h = add1ts(TupleStruct(72));
    let i = unifyplus1(SimpleEnum::Two(11));
    let j = add1ue(UnivariantEnum::Single(99));
    let k = sum(0, 1, 2, 3.0, 4.0);

    do_nothing();               // breakpoint
}
