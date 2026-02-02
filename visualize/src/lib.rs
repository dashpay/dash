// MIT LICENSE
//
// Copyright (c) 2021 Dash Core Group
//
// Permission is hereby granted, free of charge, to any
// person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the
// Software without restriction, including without
// limitation the rights to use, copy, modify, merge,
// publish, distribute, sublicense, and/or sell copies of
// the Software, and to permit persons to whom the Software
// is furnished to do so, subject to the following
// conditions:
//
// The above copyright notice and this permission notice
// shall be included in all copies or substantial portions
// of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
// ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
// TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
// PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
// SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
// CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
// IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.

//! Visualize

#![deny(missing_docs)]

use core::fmt;
use std::io::{Result, Write};

use itertools::Itertools;

static HEX_LEN: usize = 8;
static STR_LEN: usize = 32;
static INDENT_SPACES: usize = 4;

/// Pretty visualization of GroveDB components.
pub trait Visualize {
    /// Visualize
    fn visualize<W: Write>(&self, drawer: Drawer<W>) -> Result<Drawer<W>>;
}

/// Wrapper struct with a `Debug` implementation to represent bytes vector in
/// human-friendly way.
#[derive(PartialOrd, Ord, PartialEq, Eq, Hash)]
pub struct DebugBytes(pub Vec<u8>);

impl fmt::Debug for DebugBytes {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let mut v = Vec::new();
        visualize_to_vec(&mut v, self.0.as_slice());

        f.write_str(&String::from_utf8_lossy(&v))
    }
}

/// Wrapper struct with a `Debug` implementation to represent vector of bytes
/// vectors in human-friendly way.
#[derive(PartialOrd, Ord, PartialEq, Eq, Hash)]
pub struct DebugByteVectors(pub Vec<Vec<u8>>);

impl fmt::Debug for DebugByteVectors {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let mut v = Vec::new();
        let mut drawer = Drawer::new(&mut v);

        drawer.write(b"[ ").expect("write to a vector");

        for v in self.0.iter() {
            drawer = v.visualize(drawer).expect("write to a vector");
            drawer.write(b", ").expect("write to a vector");
        }

        drawer.write(b" ]").expect("write to a vector");

        f.write_str(&String::from_utf8_lossy(&v))
    }
}

/// A `io::Write` proxy to prepend padding and symbols to draw trees
pub struct Drawer<W: Write> {
    level: usize,
    write: W,
}

impl<W: Write> Drawer<W> {
    /// New
    pub fn new(write: W) -> Self {
        Drawer { level: 0, write }
    }

    /// Down
    pub fn down(&mut self) {
        self.level += 1;
    }

    /// Up
    pub fn up(&mut self) {
        self.level -= 1;
    }

    /// Write
    pub fn write(&mut self, buf: &[u8]) -> Result<()> {
        let lines_iter = buf.split(|c| *c == b'\n');
        let sep = if self.level > 0 {
            let mut result = " ".repeat(INDENT_SPACES * self.level - 1);
            result.insert(0, '\n');
            result
        } else {
            String::new()
        };
        let interspersed_lines_iter = Itertools::intersperse(lines_iter, sep.as_bytes());
        for line in interspersed_lines_iter {
            self.write.write_all(line)?;
        }
        Ok(())
    }

    /// Flush
    pub fn flush(&mut self) -> Result<()> {
        self.write.write_all(b"\n")?;
        self.write.flush()?;
        Ok(())
    }
}

/// To hex
pub fn to_hex(bytes: &[u8]) -> String {
    let encoded = hex::encode(bytes);
    let remaining = encoded.len().saturating_sub(HEX_LEN);
    if remaining >= 8 {
        format!("{}..{}", &encoded[0..HEX_LEN], &encoded[remaining..])
    } else {
        encoded
    }
}

impl Visualize for [u8] {
    fn visualize<'a, W: Write>(&self, mut drawer: Drawer<W>) -> Result<Drawer<W>> {
        let hex_repr = to_hex(self);
        let str_repr = String::from_utf8(self.to_vec());
        drawer.write(format!("[hex: {hex_repr}").as_bytes())?;
        if let Ok(str_repr) = str_repr {
            let str_part = if str_repr.len() > STR_LEN {
                &str_repr[..=STR_LEN]
            } else {
                &str_repr
            };
            drawer.write(format!(", str: {str_part}").as_bytes())?;
        }
        drawer.write(b"]")?;
        Ok(drawer)
    }
}

impl Visualize for Vec<u8> {
    fn visualize<W: Write>(&self, drawer: Drawer<W>) -> Result<Drawer<W>> {
        self.as_slice().visualize(drawer)
    }
}

impl<T: Visualize + ?Sized> Visualize for &T {
    fn visualize<'a, W: Write>(&self, drawer: Drawer<W>) -> Result<Drawer<W>> {
        (*self).visualize(drawer)
    }
}

impl<T: Visualize> Visualize for Option<T> {
    fn visualize<'a, W: Write>(&self, mut drawer: Drawer<W>) -> Result<Drawer<W>> {
        Ok(if let Some(v) = self {
            v.visualize(drawer)?
        } else {
            drawer.write(b"None")?;
            drawer
        })
    }
}

/// `visulize` shortcut to write straight into stderr offhand
pub fn visualize_stderr<T: Visualize + ?Sized>(value: &T) {
    let mut out = std::io::stderr();
    let drawer = Drawer::new(&mut out);
    value
        .visualize(drawer)
        .expect("IO error when trying to `visualize`");
}

/// `visualize` shortcut to write straight into stdout offhand
pub fn visualize_stdout<T: Visualize + ?Sized>(value: &T) {
    let mut out = std::io::stdout();
    let drawer = Drawer::new(&mut out);
    value
        .visualize(drawer)
        .expect("IO error when trying to `visualize`");
}

/// `visualize` shortcut to write into provided buffer, should be a `Vec` not a
/// slice because slices won't grow if needed.
pub fn visualize_to_vec<T: Visualize + ?Sized>(v: &mut Vec<u8>, value: &T) {
    let drawer = Drawer::new(v);
    value
        .visualize(drawer)
        .expect("error while writing into slice");
}
