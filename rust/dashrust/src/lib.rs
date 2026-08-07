// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//! Umbrella crate bundling all Rust components linked into Dash Core.
//!
//! Each component keeps its own `#[cxx::bridge]`; the explicit `extern crate`
//! declarations root the component crates so their exported bridge symbols
//! survive into the staticlib.

extern crate chirp;
