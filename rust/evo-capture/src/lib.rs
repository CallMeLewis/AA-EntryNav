// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//! evo-capture's parser, summary and pcap writer, ported from tools/evo_capture.cpp. The summary text and the pcap
//! bytes must stay identical to the C++'s: tests/golden.rs compares them with what tests/fixtures/evo_capture recorded
//! of the C++ (tests/test_capture_golden.cpp), and tests/run_qemu.py compares both programs' --selftest on ARM.
//!
//! Every length here comes from the network. Like the C++, each one is checked by subtraction, so that a huge value
//! can't wrap the car's 32-bit usize, and every read goes through a bounds-checked slice.

use std::collections::{BTreeMap, BTreeSet};
use std::io::{self, Write};

/// Where the recordings go on the car.
pub const OUT_DIR: &str = "/tmp/entry-aa-releve";
/// Bytes of each packet kept in the pcap.
pub const SNAP: usize = 2048;
pub const DEFAULT_SECONDS: i64 = 240;
pub const MAX_SECONDS: i64 = 900;
/// The pcap closes at this size; the summary goes on.
pub const MAX_BYTES: i64 = 24 * 1024 * 1024;

fn be16(p: &[u8]) -> u32 {
    u32::from(p[0]) << 8 | u32::from(p[1])
}

fn be32(p: &[u8]) -> u32 {
    u32::from(p[0]) << 24 | u32::from(p[1]) << 16 | u32::from(p[2]) << 8 | u32::from(p[3])
}

fn ip_text(p: &[u8]) -> String {
    format!("{}.{}.{}.{}", p[0], p[1], p[2], p[3])
}

/// At most `max` bytes in lower-case hex, with ".." when there were more.
fn hex(p: &[u8], max: usize) -> String {
    let mut s: String = p.iter().take(max).map(|b| format!("{b:02x}")).collect();
    if p.len() > max {
        s.push_str("..");
    }
    s
}

fn type_name(t: u32) -> &'static str {
    match t & !0x20 {
        // 0x20 is the TP flag
        0x00 => "REQUEST",
        0x01 => "REQUEST_NO_RETURN",
        0x02 => "NOTIFICATION",
        0x80 => "RESPONSE",
        0x81 => "ERROR",
        _ => "?",
    }
}

fn sd_entry_name(t: u8, ttl: u32) -> &'static str {
    match (t, ttl != 0) {
        (0x00, _) => "FIND",
        (0x01, true) => "OFFER",
        (0x01, false) => "STOP_OFFER",
        (0x06, true) => "SUBSCRIBE",
        (0x06, false) => "STOP_SUBSCRIBE",
        (0x07, true) => "SUBSCRIBE_ACK",
        (0x07, false) => "SUBSCRIBE_NACK",
        _ => "?",
    }
}

/// One (service, method, type) seen: how often, how many bytes, up to 6 flows and 3 sample payloads.
#[derive(Default, Debug)]
pub struct Pair {
    pub count: u64,
    pub bytes: u64,
    pub flows: BTreeSet<String>,
    pub samples: Vec<String>,
}

/// What the recording saw. The maps are ordered by their keys' bytes, as the C++'s std::map of std::string is.
#[derive(Default, Debug)]
pub struct Summary {
    pub per_if: BTreeMap<String, u64>,
    /// "svc 0x.... method 0x.... TYPE (iface vN)"
    pub pairs: BTreeMap<String, Pair>,
    /// One line per distinct SD entry, with its endpoints.
    pub sd: BTreeMap<String, u64>,
    pub ipv4: u64,
    pub someip: u64,
    pub skipped: u64,
}

impl Summary {
    /// The SOME/IP messages of one UDP payload or TCP segment. A TCP segment may end inside a message.
    pub fn add_some_ip(&mut self, flow: &str, mut p: &[u8], tcp: bool) {
        while p.len() >= 16 {
            let (svc, method, len) = (be16(p), be16(&p[2..]), be32(&p[4..]));
            if p[12] != 0x01 || !(8..=0x10_0000).contains(&len) {
                return; // protocol version 1 only
            }
            let total = 8 + len as usize;
            if !tcp && total > p.len() {
                return;
            }
            let have = total.min(p.len());
            let kind = u32::from(p[14]);
            self.someip += 1;
            if svc == 0xffff && method == 0x8100 {
                self.add_sd(flow, &p[16..have]);
            }
            let key = format!(
                "svc 0x{svc:04x} method 0x{method:04x} {} (iface v{})",
                type_name(kind),
                p[13]
            );
            let entry = self.pairs.entry(key).or_default();
            entry.count += 1;
            entry.bytes += total as u64;
            if entry.flows.len() < 6 {
                entry.flows.insert(flow.to_string());
            }
            if entry.samples.len() < 3 {
                entry.samples.push(hex(&p[16..have], 96));
            }
            if have < total {
                return;
            }
            p = &p[total..];
        }
    }

    /// A SOME/IP-SD payload: its entries, each with the endpoints its option indexes name.
    pub fn add_sd(&mut self, flow: &str, p: &[u8]) {
        let n = p.len();
        if n < 8 {
            return;
        }
        let elen = be32(&p[4..]) as usize;
        if n < 12 || elen > n - 12 {
            return;
        }
        let entries = &p[8..8 + elen];
        let opts_at = 8 + elen + 4;
        let mut olen = be32(&p[8 + elen..]) as usize;
        if olen > n - opts_at {
            olen = 0;
        }
        let opts = &p[opts_at..opts_at + olen];
        let mut endpoints = Vec::new();
        let mut o = 0;
        while o + 3 <= olen {
            let (l, t) = (be16(&opts[o..]) as usize, opts[o + 2]);
            if o + 3 + l > olen {
                break;
            }
            if (t == 0x04 || t == 0x14) && l == 9 {
                // IPv4 endpoint or multicast: IPv4[4] reserved[1] L4 protocol[1] port[2]
                let q = &opts[o + 4..o + 12];
                let proto = match q[5] {
                    6 => "tcp",
                    17 => "udp",
                    _ => "?",
                };
                endpoints.push(format!("{proto} {}:{}", ip_text(q), be16(&q[6..])));
            } else {
                endpoints.push(format!("opt-type-0x{}", hex(&opts[o + 2..o + 3], 1)));
            }
            o += 3 + l;
        }
        for q in entries.chunks_exact(16) {
            let (t, i1, i2) = (q[0], usize::from(q[1]), usize::from(q[2]));
            let (n1, n2) = (usize::from(q[3] >> 4), usize::from(q[3] & 15));
            let ttl = be32(&q[8..]) & 0xff_ffff;
            let head = if t == 0x06 || t == 0x07 {
                format!(
                    "{:<14} svc 0x{:04x} inst 0x{:04x} major {} eventgroup 0x{:04x}",
                    sd_entry_name(t, ttl),
                    be16(&q[4..]),
                    be16(&q[6..]),
                    q[8],
                    be16(&q[14..])
                )
            } else {
                format!(
                    "{:<14} svc 0x{:04x} inst 0x{:04x} major {} minor {}",
                    sd_entry_name(t, ttl),
                    be16(&q[4..]),
                    be16(&q[6..]),
                    q[8],
                    be32(&q[12..])
                )
            };
            let mut line = format!("{head}  [{flow}]");
            for endpoint in endpoints
                .iter()
                .skip(i1)
                .take(n1)
                .chain(endpoints.iter().skip(i2).take(n2))
            {
                line.push_str(" -> ");
                line.push_str(endpoint);
            }
            *self.sd.entry(line).or_insert(0) += 1;
        }
    }

    /// One IPv4 datagram, network header first, as a SOCK_DGRAM packet socket delivers it.
    pub fn add_ipv4(&mut self, ifname: &str, p: &[u8]) {
        *self.per_if.entry(ifname.to_string()).or_insert(0) += 1;
        let mut n = p.len();
        if n < 20 || p[0] >> 4 != 4 {
            return;
        }
        self.ipv4 += 1;
        let ihl = usize::from(p[0] & 15) * 4;
        let tot = be16(&p[2..]) as usize;
        if ihl < 20 || tot < ihl {
            return;
        }
        n = n.min(tot);
        if be16(&p[6..]) & 0x1fff != 0 {
            return; // a fragment after the first
        }
        let l4 = if n > ihl { &p[ihl..n] } else { &[][..] };
        let (src, dst) = (ip_text(&p[12..]), ip_text(&p[16..]));
        if p[9] == 17 && l4.len() >= 8 {
            let flow = format!("udp {src}:{} > {dst}:{} {ifname}", be16(l4), be16(&l4[2..]));
            self.add_some_ip(&flow, &l4[8..], false);
        } else if p[9] == 6 && l4.len() >= 20 {
            let (sp, dp) = (be16(l4), be16(&l4[2..]));
            if sp == 22 || dp == 22 {
                self.skipped += 1; // our own SSH session
                return;
            }
            let off = usize::from(l4[12] >> 4) * 4;
            if off < 20 || off > l4.len() {
                return;
            }
            let flow = format!("tcp {src}:{sp} > {dst}:{dp} {ifname}");
            self.add_some_ip(&flow, &l4[off..], true);
        }
    }

    /// The text summary, exactly as the C++ prints it.
    pub fn print(&self, f: &mut impl Write) -> io::Result<()> {
        write!(f, "\n=== Interfaces (IPv4 packets seen)\n")?;
        for (name, count) in &self.per_if {
            writeln!(f, "  {name:<12} {count}")?;
        }
        writeln!(
            f,
            "IPv4 {}, SOME/IP messages {}, SSH ignored {}",
            self.ipv4, self.someip, self.skipped
        )?;
        write!(f, "\n=== SOME/IP-SD (distinct offers and subscriptions)\n")?;
        for (line, count) in &self.sd {
            writeln!(f, "  x{count:<5} {line}")?;
        }
        write!(f, "\n=== SOME/IP (service, method or event)\n")?;
        for (key, pair) in &self.pairs {
            writeln!(f, "{key}: {} messages, {} bytes", pair.count, pair.bytes)?;
            for flow in &pair.flows {
                writeln!(f, "    flow {flow}")?;
            }
            for sample in &pair.samples {
                writeln!(f, "    sample {sample}")?;
            }
        }
        Ok(())
    }
}

/// A pcap file of raw IPv4 (LINKTYPE_RAW), version 2.4, in the host's byte order, as the C++ writes it.
pub struct PcapWriter<W: Write> {
    out: W,
    /// Bytes written so far, header included.
    pub bytes: i64,
}

impl<W: Write> PcapWriter<W> {
    pub fn new(mut out: W) -> io::Result<Self> {
        for word in [0xa1b2_c3d4u32, 0x0004_0002, 0, 0, 65535, 101] {
            out.write_all(&word.to_ne_bytes())?;
        }
        Ok(PcapWriter { out, bytes: 24 })
    }

    /// One packet: its first `data.len()` bytes kept, `len` its size on the wire.
    pub fn add(&mut self, sec: u32, usec: u32, data: &[u8], len: usize) -> io::Result<()> {
        for word in [sec, usec, data.len() as u32, len as u32] {
            self.out.write_all(&word.to_ne_bytes())?;
        }
        self.out.write_all(data)?;
        self.bytes += 16 + data.len() as i64;
        Ok(())
    }

    pub fn finish(mut self) -> io::Result<W> {
        self.out.flush()?;
        Ok(self.out)
    }
}

/// One record of a pcap file.
#[derive(Debug, PartialEq, Eq)]
pub struct PcapRecord {
    pub sec: u32,
    pub usec: u32,
    pub data: Vec<u8>,
    pub len: u32,
}

/// The records of a little-endian pcap file of raw IPv4, or None if it is anything else or cut short.
pub fn read_pcap(file: &[u8]) -> Option<Vec<PcapRecord>> {
    let word = |at: usize| {
        file.get(at..at + 4)
            .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    };
    if word(0)? != 0xa1b2_c3d4 || word(20)? != 101 {
        return None;
    }
    let mut records = Vec::new();
    let mut at = 24;
    while at < file.len() {
        let (sec, usec, caplen, len) = (word(at)?, word(at + 4)?, word(at + 8)? as usize, word(at + 12)?);
        let data = file.get(at + 16..at + 16 + caplen)?.to_vec();
        records.push(PcapRecord { sec, usec, data, len });
        at += 16 + caplen;
    }
    Some(records)
}

// ---- self test (no network) -----------------------------------------------------------------------------------------

/// An IPv4 UDP datagram, as the C++ self test builds it.
pub fn udp(src: [u8; 4], sp: u16, dst: [u8; 4], dp: u16, payload: &[u8]) -> Vec<u8> {
    let mut v = vec![0u8; 28 + payload.len()];
    let size = v.len() as u16;
    v[0] = 0x45;
    v[2..4].copy_from_slice(&size.to_be_bytes());
    v[8] = 64;
    v[9] = 17;
    v[12..16].copy_from_slice(&src);
    v[16..20].copy_from_slice(&dst);
    v[20..22].copy_from_slice(&sp.to_be_bytes());
    v[22..24].copy_from_slice(&dp.to_be_bytes());
    v[24..26].copy_from_slice(&(8 + payload.len() as u16).to_be_bytes());
    v[28..].copy_from_slice(payload);
    v
}

/// A SOME/IP message, protocol and interface version 1.
pub fn someip(svc: u16, method: u16, kind: u8, body: &[u8]) -> Vec<u8> {
    let mut v = vec![0u8; 16 + body.len()];
    v[0..2].copy_from_slice(&svc.to_be_bytes());
    v[2..4].copy_from_slice(&method.to_be_bytes());
    v[4..8].copy_from_slice(&(8 + body.len() as u32).to_be_bytes());
    v[12] = 1;
    v[13] = 1;
    v[14] = kind;
    v[16..].copy_from_slice(body);
    v
}

/// The parser's check on synthetic packets, with the C++'s output, byte for byte. True when it passed.
pub fn selftest(out: &mut impl Write) -> io::Result<bool> {
    let (hu, kombi, mc) = ([160, 48, 199, 119], [160, 48, 199, 20], [224, 244, 224, 245]);
    let mut s = Summary::default();
    // SD offer from the HU with an IPv4 UDP endpoint option.
    let mut sd = vec![0u8; 4 + 4 + 16 + 4 + 12];
    sd[0] = 0xc0;
    sd[7] = 16; // flags, entries length 16
    sd[8..20].copy_from_slice(&[0x01, 0, 0, 0x10, 0x12, 0x34, 0, 1, 1, 0, 0, 3]);
    sd[27] = 12; // options length 12
    sd[29] = 9;
    sd[30] = 0x04;
    sd[32..36].copy_from_slice(&hu);
    sd[37] = 17;
    sd[38] = 0x77;
    sd[39] = 0x5e; // udp 160.48.199.119:30558
    s.add_ipv4(
        "vbr0.73",
        &udp(hu, 30490, mc, 30490, &someip(0xffff, 0x8100, 0x02, &sd)),
    );
    // Two notifications in one UDP datagram (event 0x8001 of service 0x1234) to the cluster.
    let mut two = someip(0x1234, 0x8001, 0x02, &[0xab; 5]);
    two.extend(someip(0x1234, 0x8001, 0x02, &[0xcd; 3]));
    s.add_ipv4("vbr0.73", &udp(hu, 30558, kombi, 30501, &two));
    // Garbage UDP (not SOME/IP) is ignored.
    s.add_ipv4("vbr0.73", &udp(kombi, 5000, hu, 5001, &[0x55; 20]));
    let first_sd = s.sd.keys().next().cloned().unwrap_or_default();
    let mut ok = s.someip == 3
        && s.sd.len() == 1
        && s.pairs
            .get("svc 0x1234 method 0x8001 NOTIFICATION (iface v1)")
            .is_some_and(|p| p.count == 2)
        && first_sd.starts_with("OFFER")
        && first_sd.contains("udp 160.48.199.119:30558")
        && first_sd.contains("svc 0x1234 inst 0x0001");
    s.print(out)?;
    // Lengths that would wrap a 32-bit usize (the car's ARM): entries refused, options ignored, nothing read past the
    // packet.
    let mut w = Summary::default();
    let mut bad = sd.clone();
    bad[4..8].copy_from_slice(&[0xff, 0xff, 0xff, 0xf8]); // entries length 0xfffffff8
    w.add_ipv4(
        "vbr0.73",
        &udp(hu, 30490, mc, 30490, &someip(0xffff, 0x8100, 0x02, &bad)),
    );
    let entries_refused = w.someip == 1 && w.sd.is_empty();
    let mut bad = sd.clone();
    bad[24..28].copy_from_slice(&[0xff, 0xff, 0xff, 0xfc]); // options length 0xfffffffc
    w.add_ipv4(
        "vbr0.73",
        &udp(hu, 30490, mc, 30490, &someip(0xffff, 0x8100, 0x02, &bad)),
    );
    let first_w = w.sd.keys().next().cloned().unwrap_or_default();
    let options_ignored = w.sd.len() == 1 && first_w.starts_with("OFFER") && !first_w.contains(" -> ");
    ok = ok && entries_refused && options_ignored;
    if ok {
        writeln!(
            out,
            "PASS evo-capture selftest (SD offer + endpoint, 2 SOME/IP in one datagram, non-SOME/IP ignored, \
             wrapping SD lengths refused)"
        )?;
    } else {
        writeln!(out, "FAIL evo-capture selftest")?;
    }
    Ok(ok)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hex_marks_what_it_cut() {
        assert_eq!(hex(&[0xab, 0x01], 2), "ab01");
        assert_eq!(hex(&[0xab, 0x01, 0x02], 2), "ab01..");
        assert_eq!(hex(&[], 96), "");
    }

    #[test]
    fn tp_flag_keeps_the_type() {
        assert_eq!(type_name(0x22), "NOTIFICATION");
        assert_eq!(type_name(0xa0), "RESPONSE");
        assert_eq!(type_name(0x03), "?");
    }

    #[test]
    fn short_or_foreign_packets_count_only_their_interface() {
        let mut s = Summary::default();
        s.add_ipv4("eth0", &[0x45; 10]);
        s.add_ipv4("eth0", &[0x60; 40]);
        assert_eq!((s.per_if["eth0"], s.ipv4), (2, 0));
    }

    #[test]
    fn a_tcp_segment_may_end_inside_a_message() {
        let mut s = Summary::default();
        let mut cut = someip(0x1234, 1, 0, &[1; 40]);
        cut.truncate(30);
        s.add_some_ip("tcp", &cut, true);
        let pair = &s.pairs["svc 0x1234 method 0x0001 REQUEST (iface v1)"];
        assert_eq!((pair.count, pair.bytes, pair.samples[0].len()), (1, 56, 28));
        let mut u = Summary::default();
        u.add_some_ip("udp", &cut, false);
        assert_eq!(u.someip, 0);
    }

    #[test]
    fn pcap_reads_back_what_it_wrote() {
        let mut w = PcapWriter::new(Vec::new()).unwrap();
        w.add(7, 1, &[1, 2, 3], 9).unwrap();
        assert_eq!(w.bytes, 24 + 16 + 3);
        let file = w.finish().unwrap();
        let records = read_pcap(&file).unwrap();
        assert_eq!(
            records,
            vec![PcapRecord {
                sec: 7,
                usec: 1,
                data: vec![1, 2, 3],
                len: 9
            }]
        );
        assert!(read_pcap(&file[..file.len() - 1]).is_none());
    }
}
