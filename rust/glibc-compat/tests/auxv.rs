// Copyright (c) 2026 CallMeLewis
// The auxiliary vector reader behind glibc-compat's getauxval, on 32-bit words as on the car and on 64-bit ones.

use glibc_compat::auxv_entry;

fn vector(word: usize, pairs: &[(usize, usize)]) -> Vec<u8> {
    let mut out = Vec::new();
    for &(key, value) in pairs.iter().chain([(0, 0)].iter()) {
        out.extend_from_slice(&(key as u64).to_le_bytes()[..word]);
        out.extend_from_slice(&(value as u64).to_le_bytes()[..word]);
    }
    out
}

#[test]
fn finds_an_entry_on_32_and_64_bits() {
    // 64-bit words only where usize holds them: on the car's ARM it is 32 bits.
    for word in [4, 8].into_iter().filter(|&w| w <= std::mem::size_of::<usize>()) {
        let auxv = vector(word, &[(16, 0x1234_5678), (6, 4096), (51, 2048)]);
        assert_eq!(auxv_entry(&auxv, 16, word), Some(0x1234_5678));
        assert_eq!(auxv_entry(&auxv, 51, word), Some(2048));
        assert_eq!(auxv_entry(&auxv, 6, word), Some(4096));
    }
}

#[test]
fn a_missing_entry_or_a_cut_vector_gives_nothing() {
    let auxv = vector(4, &[(16, 1), (6, 4096)]);
    assert_eq!(auxv_entry(&auxv, 51, 4), None);
    assert_eq!(auxv_entry(&auxv[..12], 6, 4), None);
    assert_eq!(auxv_entry(&[], 6, 4), None);
}
