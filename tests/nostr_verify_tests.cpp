// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
// Official BIP-340 vectors and NIP-01 serialization smoke tests; built once
// per available secp256k1 backend (OpenSSL, libsecp256k1 + Mbed TLS).
#include "xanatoo/nostr_verify.hpp"
#include "xanatoo/nostr_nip11.hpp"
#include "xanatoo/nostr_sign.hpp"
#include "xanatoo/json_min.hpp"
#include <cassert>
#include <iostream>
#include <string>

int main() {
  using namespace xanatoo;
  // Standalone NIP-01 fixture signed with the public BIP-340 test private
  // key 3. Checked against an independent Python big-integer implementation.
  const std::string signed_event =
      R"({"id":"da67626634b798564d957125c4105766d6a681ff62eb48a87f5ec4e819ff0b0b","pubkey":"f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9","created_at":123,"kind":1,"tags":[],"content":"hello","sig":"2d5ebb7dc3a294651cbe7ff4d3fe01819ce4ec5f1676e1af0133196cb8a66981ec1ae0a2c01fc6684241556dba3d54ab778ca4ce9c2e0379eb300d0a2f657643"})";
  assert(nostr_event_id(signed_event) ==
         "da67626634b798564d957125c4105766d6a681ff62eb48a87f5ec4e819ff0b0b");
  // Independent NIP-29 kind-39000 fixture signed by the official BIP-340
  // private test key 3. A matching relay "self" is still insufficient when
  // the crypto verifier is missing, or the HTTPS NIP-11 response was not
  // authenticated; only the real caller may pass a verified relay self.
  const std::string signed_group =
      R"({"id":"57be22489918c23cc660b9aaf61d9c871cf28950821da6bcd2e75efdf360548f","pubkey":"f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9","created_at":321,"kind":39000,"tags":[["d","verified-group"],["name","Verified NIP29"]],"content":"","sig":"334111a62a0863f7421baf17d6391e5801f6dfa3f591246c3236976e103f8d0cde5aea92eebb0082283067df8de03c061db66a12e407d73da797f36123929671"})";
  assert(nostr_event_id(signed_group) ==
         "57be22489918c23cc660b9aaf61d9c871cf28950821da6bcd2e75efdf360548f");
  const std::string relay_self =
      "f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9";
#if XANATOO_NOSTR_CRYPTO
  assert(nostr_verify_event(signed_event) == NostrVerification::SignatureValid);
  assert(nostr_verify_event(signed_group) == NostrVerification::SignatureValid);
  assert(nostr_relay_master_matches(relay_self, relay_self,
                                   nostr_verify_event(signed_group) ==
                                       NostrVerification::SignatureValid));
#else
  assert(nostr_verify_event(signed_event) == NostrVerification::HashOnly);
  assert(nostr_verify_event(signed_group) == NostrVerification::HashOnly);
  assert(!nostr_relay_master_matches(relay_self, relay_self,
                                    nostr_verify_event(signed_group) ==
                                        NostrVerification::SignatureValid));
#endif
  std::string altered = signed_event;
  altered.replace(altered.find("hello"), 5, "hallo");
  assert(nostr_verify_event(altered) == NostrVerification::Invalid);
  const std::string pubkey =
      "F9308A019258C31049344F85F89D5229B531C845836F99B08601F113BCE036F9";
  const std::string zero(64, '0');
  const std::string valid =
      "E907831F80848D1069A5371B402410364BDF1C5F8307B0084C55F1CE2DCA8215"
      "25F66A4A85EA8B71E482A74F382D2CE5EBEEE8FDB2172F477DF4900D310536C0";
#if XANATOO_NOSTR_CRYPTO
  assert(nostr_verify_bip340(pubkey, zero, valid));  // vector 0
  assert(!nostr_verify_bip340(pubkey, zero, zero + zero));
  assert(!nostr_verify_bip340(std::string(64, 'f'), zero, valid));
  // Official vector 6: R has odd Y.
  assert(!nostr_verify_bip340(
      "DFF1D77F2A671C5F36183726DB2341BE58FEAE1DA2DECED843240F7B502BA659",
      "243F6A8885A308D313198A2E03707344A4093822299F31D0082EFA98EC4E6C89",
      "FFF97BD5755EEEA420453A14355235D382F6472F8568A18B2F057A1460297556"
      "3CC27944640AC607CD107AE10923D9EF7A73C643E166BE5EBEAFA34B1AC553E2"));
#else
  assert(!nostr_verify_bip340(pubkey, zero, valid));
#endif

  const std::string raw = "{\"id\":\"" + zero + "\",\"pubkey\":\"" +
      std::string(64, 'a') + "\",\"created_at\":123,\"kind\":1,"
      "\"content\":\"hello\\nworld\",\"tags\":[[\"p\",\"bob\"],[\"t\",\"C++\"]],"
      "\"sig\":\"" + zero + zero + "\"}";
  const auto id = nostr_event_id(raw);
  assert(id && id->size() == 64);
  // SHA-256 of this independently serialized NIP-01 array, checked against
  // sha256sum (not generated using nostr_event_id itself).
  const std::string simple = "{\"pubkey\":\"" + std::string(64, 'a') +
      "\",\"created_at\":1,\"kind\":1,\"tags\":[],\"content\":\"hello\"}";
  assert(nostr_event_id(simple) ==
         "f67935eddb3744c8798809e3758dcbb80dd0d04400842506030ada9680bfdf5a");
  std::string populated = raw;
  populated.replace(populated.find(zero), 64, *id);
#if XANATOO_NOSTR_CRYPTO
  assert(nostr_verify_event(populated) == NostrVerification::Invalid);
#else
  assert(nostr_verify_event(populated) == NostrVerification::HashOnly);
#endif
  populated.replace(populated.find(*id), 64, zero);
  assert(nostr_verify_event(populated) == NostrVerification::Invalid);
  assert(!nostr_event_id("{\"pubkey\":\"bad\",\"tags\":[]}"));
  assert(!nostr_event_id("{\"pubkey\":\"" + std::string(64, 'a') +
                          "\",\"created_at\":1,\"kind\":1,\"content\":\"x\","
                          "\"tags\":[[\"t\",9]]}"));
  assert(nostr_event_id("{\"pubkey\":\"" + std::string(64, 'a') +
                        "\",\"created_at\":1,\"kind\":1,\"content\":\"tags\",\"tags\":[]}"));
  assert(!nostr_event_id("{\"pubkey\":\"" + std::string(64, 'a') +
                          "\",\"created_at\":1.5,\"kind\":1,\"content\":\"x\",\"tags\":[]}"));

  // ---- Signing (nostr_sign.hpp) ----
  // nsec round trip (NIP-19 example key) needs no crypto backend.
  const std::string nip19_sk =
      "67dea2ed018072d675f5415ecfaed7d2597555e202d85b3d65ea4e58d2d92ffa";
  assert(nostr_nsec(nip19_sk) ==
         "nsec1vl029mgpspedva04g90vltkh6fvh240zqtv9k0t9af8935ke9laqsnlfe5");
  assert(nostr_decode_nsec("nostr:nsec1vl029mgpspedva04g90vltkh6fvh240zqtv9k0t9af8935ke9laqsnlfe5") ==
         nip19_sk);
  assert(!nostr_decode_nsec("npub1vl029mgpspedva04g90vltkh6fvh240zqtv9k0t9af8935ke9laqsnlfe5"));
  const std::string nip49 =
      "ncryptsec1qgg9947rlpvqu76pj5ecreduf9jxhselq2nae2kghhvd5g7dgjtcxfqtd67p9m0w57lspw8gsq6yphnm8623nsl8xn9j4jdzz84zm3frztj3z7s35vpzmqf6ksu8r89qk5z2zxfmu5gv8th8wclt0h4p";
  assert(is_ncryptsec(nip49) && !is_ncryptsec(nip19_sk));
#if XANATOO_NOSTR_CRYPTO
  assert(nostr_signing_available());
  // Official BIP-340 signing vectors 0..3 (secret key, aux, message, sig).
  struct Vec { const char *sk, *pk, *aux, *msg, *sig; };
  const Vec vecs[] = {
      {"0000000000000000000000000000000000000000000000000000000000000003",
       "f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9",
       "0000000000000000000000000000000000000000000000000000000000000000",
       "0000000000000000000000000000000000000000000000000000000000000000",
       "e907831f80848d1069a5371b402410364bdf1c5f8307b0084c55f1ce2dca8215"
       "25f66a4a85ea8b71e482a74f382d2ce5ebeee8fdb2172f477df4900d310536c0"},
      {"B7E151628AED2A6ABF7158809CF4F3C762E7160F38B4DA56A784D9045190CFEF",
       "dff1d77f2a671c5f36183726db2341be58feae1da2deced843240f7b502ba659",
       "0000000000000000000000000000000000000000000000000000000000000001",
       "243F6A8885A308D313198A2E03707344A4093822299F31D0082EFA98EC4E6C89",
       "6896bd60eeae296db48a229ff71dfe071bde413e6d43f917dc8dcf8c78de3341"
       "8906d11ac976abccb20b091292bff4ea897efcb639ea871cfa95f6de339e4b0a"},
      {"C90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B14E5C9",
       "dd308afec5777e13121fa72b9cc1b7cc0139715309b086c960e18fd969774eb8",
       "C87AA53824B4D7AE2EB035A2B5BBBCCC080E76CDC6D1692C4B0B62D798E6D906",
       "7E2D58D8B3BCDF1ABADEC7829054F90DDA9805AAB56C77333024B9D0A508B75C",
       "5831aaeed7b44bb74e5eab94ba9d4294c49bcf2a60728d8b4c200f50dd313c1b"
       "ab745879a5ad954a72c45a91c3a51d3c7adea98d82f8481e0e1e03674a6f3fb7"},
      {"0B432B2677937381AEF05BB02A66ECD012773062CF3FA2549E44F58ED2401710",
       "25d1dff95105f5253c4022f628a996ad3a0d95fbf21d468a1b33f8c160d8f517",
       "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",
       "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",
       "7eb0509757e246f19449885651611cb965ecc1a187dd51b64fda1edc9637d5ec"
       "97582b9cb13db3933705b32ba982af5af25fd78881ebb32771fc5922efc66ea3"},
  };
  for (const Vec& v : vecs) {
    assert(nostr_pubkey_of(v.sk) == v.pk);
    assert(nostr_sign_bip340(v.sk, v.msg, v.aux) == v.sig);
  }
  assert(nostr_pubkey_of(std::string(64, '0')).empty());
  assert(nostr_pubkey_of(  // the curve order n is not a key
             "fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141")
             .empty());

  // Fresh keys sign events the verifier accepts.
  const std::string sk = nostr_generate_secret();
  assert(sk.size() == 64 && sk != nostr_generate_secret());
  const std::string pk = nostr_pubkey_of(sk);
  const std::string ev = nostr_sign_event(
      sk, 1, "hello \"nostr\"\nfrom xanatoo ✓", {{"t", "xanatoo"}, {"client", "x"}},
      1700000000);
  assert(!ev.empty());
  assert(nostr_verify_event(ev) == NostrVerification::SignatureValid);
  assert(json_str_field(ev, "pubkey") == pk);
  assert(json_str_field(ev, "content") == "hello \"nostr\"\nfrom xanatoo ✓");
  std::string forged = ev;
  forged.replace(forged.find("hello"), 5, "jello");
  assert(nostr_verify_event(forged) == NostrVerification::Invalid);

  // NIP-49 spec vector, then a round trip (cheap log_n for the test).
  std::string err;
  assert(nostr_ncryptsec_decrypt(nip49, "nostr", &err) ==
         "3501454135014541350145413501453fefb02227e449e57cf4d3a3ce05378683");
  assert(!nostr_ncryptsec_decrypt(nip49, "nostR", &err) && err == "wrong password");
  const std::string enc = nostr_ncryptsec_encrypt(sk, "pässwörd", 4, 1);
  assert(is_ncryptsec(enc) && enc.size() > 150);
  assert(enc != nostr_ncryptsec_encrypt(sk, "pässwörd", 4, 1));  // fresh salt
  assert(nostr_ncryptsec_decrypt(enc, "pässwörd") == sk);
  assert(!nostr_ncryptsec_decrypt(enc, "password"));
  std::string tampered = enc;
  tampered[tampered.size() - 10] = tampered[tampered.size() - 10] == 'q' ? 'p' : 'q';
  assert(!nostr_ncryptsec_decrypt(tampered, "pässwörd"));
#else
  assert(!nostr_signing_available());
  assert(nostr_generate_secret().empty());
  assert(!nostr_ncryptsec_decrypt(nip49, "nostr"));
#endif
  std::cout << "NIP-01/BIP-340 verifier smoke: PASS\n";
}
