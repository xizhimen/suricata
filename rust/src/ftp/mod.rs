/* Copyright (C) 2017 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

extern crate libc;
extern crate nom;

use nom::{digit};
use std::str;
use std;
use std::str::FromStr;

// We transform an integer string into a i64, ignoring surrounding whitespaces
// We look for a digit suite, and try to convert it.
// If either str::from_utf8 or FromStr::from_str fail,
// we fallback to the parens parser defined above
named!(getu16<u16>,
    map_res!(
      map_res!(
        ws!(digit),
        str::from_utf8
      ),
      FromStr::from_str
    )
);

// 227 Entering Passive Mode (212,27,32,66,221,243).
named!(pub ftp_pasv_response<u16>,
       do_parse!(
            tag!("227") >>
            take_until_and_consume!("(") >>
            digit >> tag!(",") >> digit >> tag!(",") >>
            digit >> tag!(",") >> digit >> tag!(",") >>
            part1: getu16 >>
            tag!(",") >>
            part2: getu16 >>
            tag!(").") >>
            (
                part1 * 256 + part2
            )
        )
);


#[no_mangle]
pub extern "C" fn rs_ftp_pasv_response(input: *const libc::uint8_t, len: libc::uint32_t) -> u16 {
    let buf = unsafe{std::slice::from_raw_parts(input, len as usize)};
    match ftp_pasv_response(buf) {
        nom::IResult::Done(_, dport) => {
            return dport;
        }
        nom::IResult::Incomplete(_) => { println!("incomplete"); },
        nom::IResult::Error(_) => { println!("error"); },
    }
    return 0;
}
