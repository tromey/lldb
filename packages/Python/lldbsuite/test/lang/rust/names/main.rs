#![allow(dead_code)]
#![allow(unused_variables)]
#![allow(unused_assignments)]

fn do_nothing() { }

pub static VALUE : u16 = 7777;

pub mod m1 {
    pub static VALUE : u16 = 1;

    pub mod m1_1 {
        pub static VALUE : u16 = 11;
    }

    pub mod m1_2 {
        pub static VALUE : u16 = 12;
    }
}

pub mod m2 {
    pub static VALUE : u16 = 2;

    pub mod m2_1 {
        pub static VALUE : u16 = 21;

        pub mod m2_1_2 {
            pub static VALUE : u16 = 212;

            pub mod m2_1_2_1 {
                pub static VALUE : u16 = 2121;
            }

            pub fn f() {
                let v0 = ::VALUE;
                let v1 = ::m1::VALUE;
                let v1_1 = ::m1::m1_1::VALUE;
                let v1_2 = ::m1::m1_2::VALUE;
                let v2 = ::m2::VALUE;
                let v2_1 = ::m2::m2_1::VALUE;
                let v2_1_2 = ::m2::m2_1::m2_1_2::VALUE;
                let v22 = ::m2::m2_2::VALUE;

                let v = VALUE;
                let svalue = self::VALUE;
                let suvalue = super::VALUE;
                let ssuvalue = self::super::VALUE;
                let susuvalue = super::super::VALUE;
                let ssusuvalue = self::super::super::VALUE;
                let sususuvalue = super::super::super::VALUE;
                let ssususuvalue = self::super::super::super::VALUE;

                let rv = m2_1_2_1::VALUE;
                let srv = self::m2_1_2_1::VALUE;
                let ssrv = super::m2_1_2::m2_1_2_1::VALUE;

                ::do_nothing();   // breakpoint
            }
        }
    }

    pub mod m2_2 {
        pub static VALUE : u16 = 22;
    }
}

fn main() {
    m2::m2_1::m2_1_2::f();
}
