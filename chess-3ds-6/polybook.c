/* polybook.c from BrainFish, Copyright (C) 2016-2017 Thomas Zipproth */

#include <stdio.h>
#include <string.h>

#include "misc.h"
#include "movegen.h"
#include "polybook.h"
#include "thread.h"
#include "types.h"
#include "uci.h"
#include "3ds_bridge.h"

// --- HARDWARE-SAFE EMBEDDED DATA EMBEDDING ---
#ifdef USE_EMBEDDED_BOOK
#include <stdint.h>

__asm__(
  ".section .rodata, \"a\", %progbits\n"
  ".balign 8\n"
  ".global _binary_Best_bin_start\n"
  "_binary_Best_bin_start:\n"
  ".incbin \"Best.bin\"\n"
  "_binary_Best_bin_end:\n"
  ".balign 4\n"
  ".global _binary_Best_bin_size\n"
  "_binary_Best_bin_size:\n"
  ".int _binary_Best_bin_end - _binary_Best_bin_start\n"
  ".previous\n"
);

// __attribute__((used)) guarantees LTO does not optimize away these symbols
extern const uint8_t _binary_Best_bin_start[] __attribute__((aligned(8), used));
extern const uint32_t _binary_Best_bin_size __attribute__((used));
#endif
// ----------------------------------------------

struct PolyHash {
  uint64_t key;
  uint16_t move;
  uint16_t weight;
  uint32_t learn;
};

// --- ALIGNMENT-SAFE HELPER READERS ---
// Using memcpy avoids hardware Alignment Faults on ARMv6/ARM11 architectures.
// Compilers inline these directly into hardware registers with zero execution lag.
static inline uint64_t read_key(const struct PolyHash *entry) {
    uint64_t val;
    memcpy(&val, &entry->key, sizeof(uint64_t));
    return from_be_u64(val);
}

static inline uint64_t read_raw_key(const struct PolyHash *entry) {
    uint64_t val;
    memcpy(&val, &entry->key, sizeof(uint64_t));
    return val;
}

static inline uint16_t read_move(const struct PolyHash *entry) {
    uint16_t val;
    memcpy(&val, &entry->move, sizeof(uint16_t));
    return from_be_u16(val);
}

static inline uint16_t read_weight(const struct PolyHash *entry) {
    uint16_t val;
    memcpy(&val, &entry->weight, sizeof(uint16_t));
    return from_be_u16(val);
}
// -------------------------------------

static Key polyglot_key(const Position *pos);
static Move pg_move_to_sf_move(const Position *pos, uint16_t pg_move);

static int find_first_key(PolyBook *pb, uint64_t key);
static int get_key_data(PolyBook *pb);

static bool check_do_search(PolyBook *pb, const Position *pos);
static bool check_draw(Position *pos, Move m);

// Random numbers from PolyGlot, used to compute book hash keys
static const union {
  uint64_t PolyGlotRandoms[781];
  struct {
    uint64_t psq[12][64];  // [piece][square]
    uint64_t castle[4];    // [castle right]
    uint64_t enpassant[8]; // [file]
    uint64_t turn;
  } Zobrist;
} PG = { {
  0x9D39247E33776D41ULL, 0x2AF7398005AAA5C7ULL, 0x44DB015024623547ULL,
  0x9C15F73E62A76AE2ULL, 0x75834465489C0C89ULL, 0x3290AC3A203001BFULL,
  0x0FBBAD1F61042279ULL, 0xE83A908FF2FB60CAULL, 0x0D7E765D58755C10ULL,
  0x1A083822CEAFE02DULL, 0x9605D5F0E25EC3B0ULL, 0xD021FF5CD13A2ED5ULL,
  0x40BDF15D4A672E32ULL, 0x011355146FD56395ULL, 0x5DB4832046F3D9E5ULL,
  0x239F8B2D7FF719CCULL, 0x05D1A1AE85B49AA1ULL, 0x679F848F6E8FC971ULL,
  0x7449BBFF801FED0BULL, 0x7D11CDB1C3B7ADF0ULL, 0x82C7709E781EB7CCULL,
  0xF3218F1C9510786CULL, 0x331478F3AF51BBE6ULL, 0x4BB38DE5E7219443ULL,
  0xAA649C6EBCFD50FCULL, 0x8DBD98A352AFD40BULL, 0x87D2074B81D79217ULL,
  0x19F3C751D3E92AE1ULL, 0xB4AB30F062B19ABFULL, 0x7B0500AC42047AC4ULL,
  0xC9452CA81A09D85DULL, 0x24AA6C514DA27500ULL, 0x4C9F34427501B447ULL,
  0x14A68FD73C910841ULL, 0xA71B9B83461CBD93ULL, 0x03488B95B0F1850FULL,
  0x637B2B34FF93C040ULL, 0x09D1BC9A3DD90A94ULL, 0x3575668334A1DD3BULL,
  0x735E2B97A4C45A23ULL, 0x18727070F1BD400BULL, 0x1FCBACD259BF02E7ULL,
  0xD310A7C2CE9B6555ULL, 0xBF983FE0FE5D8244ULL, 0x9F74D14F7454A824ULL,
  0x51EBDC4AB9BA3035ULL, 0x5C82C505DB9AB0FAULL, 0xFCF7FE8A3430B241ULL,
  0x3253A729B9BA3DDEULL, 0x8C74C368081B3075ULL, 0xB9BC6C87167C33E7ULL,
  0x7EF48F2B83024E20ULL, 0x11D505D4C351BD7FULL, 0x6568FCA92C76A243ULL,
  0x4DE0B0F40F32A7B8ULL, 0x96D693460CC37E5DULL, 0x42E240CB63689F2FULL,
  0x6D2BDCDAE2919661ULL, 0x42880B0236E4D951ULL, 0x5F0F4A5898171BB6ULL,
  0x39F890F579F92F88ULL, 0x93C5B5F47356388BULL, 0x63DC359D8D231B78ULL,
  0xEC16CA8AEA98AD76ULL, 0x5355F900C2A82DC7ULL, 0x07FB9F855A997142ULL,
  0x5093417AA8A7ED5EULL, 0x7BCBC38DA25A7F3CULL, 0x19FC8A768CF4B6D4ULL,
  0x637A7780DECFC0D9ULL, 0x8249A47AEE0E41F7ULL, 0x79AD695501E7D1E8ULL,
  0x14ACBAF4777D5776ULL, 0xF145B6BECCDEA195ULL, 0xDABF2AC8201752FCULL,
  0x24C3C94DF9C8D3F6ULL, 0xBB6E2924F03912EAULL, 0x0CE26C0B95C980D9ULL,
  0xA49CD132BFBF7CC4ULL, 0xE99D662AF4243939ULL, 0x27E6AD7891165C3FULL,
  0x8535F040B9744FF1ULL, 0x54B3F4FA5F40D873ULL, 0x72B12C32127FED2BULL,
  0xEE954D3C7B411F47ULL, 0x9A85AC909A24EAA1ULL, 0x70AC4CD9F04F21F5ULL,
  0xF9B89D3E99A075C2ULL, 0x87B3E2B2B5C907B1ULL, 0xA366E5B8C54F48B8ULL,
  0xAE4A9346CC3F7CF2ULL, 0x1920C04D47267BBDULL, 0x87BF02C6B49E2AE9ULL,
  0x092237AC237F3859ULL, 0xFF07F64EF8ED14D0ULL, 0x8DE8DCA9F03CC54EULL,
  0x9C1633264DB49C89ULL, 0xB3F22C3D0B0B38EDULL, 0x390E5FB44D01144BULL,
  0x5BFEA5B4712768E9ULL, 0x1E1032911FA78984ULL, 0x9A74ACB964E78CB3ULL,
  0x4F80F7A035DAFB04ULL, 0x6304D09A0B3738C4ULL, 0x2171E64683023A08ULL,
  0x5B9B63EB9CEFF80CULL, 0x506AACF489889342ULL, 0x1881AFC9A3A701D6ULL,
  0x6503080440750644ULL, 0xDFD395339CDBF4A7ULL, 0xEF927DBCF00C20F2ULL,
  0x7B32F7D1E03680ECULL, 0xB9FD7620E7316243ULL, 0x05A7E8A57DB91B77ULL,
  0xB5889C6E15630A75ULL, 0x4A750A09CE9573F7ULL, 0xCF464CEC899A2F8AULL,
  0xF538639CE705B824ULL, 0x3C79A0FF5580EF7FULL, 0xEDE6C87F8477609DULL,
  0x799E81F05BC93F31ULL, 0x86536B8CF3428A8CULL, 0x97D7374C60087B73ULL,
  0xA246637CFF328532ULL, 0x043FCAE60CC0EBA0ULL, 0x920E449535DD359EULL,
  0x70EB093B15B290CCULL, 0x73A1921916591CBDULL, 0x56436C9FE1A1AA8DULL,
  0xEFAC4B70633B8F81ULL, 0xBB215798D45DF7AFULL, 0x45F20042F24F1768ULL,
  0x930F80F4E8EB7462ULL, 0xFF6712FFCFD75EA1ULL, 0xAE623FD67468AA70ULL,
  0xDD2C5BC84BC8D8FCULL, 0x7EED120D54CF2DD9ULL, 0x22FE545401165F1CULL,
  0xC91800E98FB99929ULL, 0x808BD68E6AC10365ULL, 0xDEC468145B7605F6ULL,
  0x1BEDE3A3AEF53302ULL, 0x43539603D6C55602ULL, 0xAA969B5C691CCB7AULL,
  0xA87832D392EFEE56ULL, 0x65942C7B3C7E11AEULL, 0xDED2D633CAD004F6ULL,
  0x21F08570F420E565ULL, 0xB415938D7DA94E3CULL, 0x91B859E59ECB6350ULL,
  0x10CFF333E0ED804AULL, 0x28AED140BE0BB7DDULL, 0xC5CC1D89724FA456ULL,
  0x5648F680F11A2741ULL, 0x2D255069F0B7DAB3ULL, 0x9BC5A38EF729ABD4ULL,
  0xEF2F054308F6A2BCULL, 0xAF2042F5CC5C2858ULL, 0x480412BAB7F5BE2AULL,
  0xAEF3AF4A563DFE43ULL, 0x19AFE59AE451497FULL, 0x52593803DFF1E840ULL,
  0xF4F076E65F2CE6F0ULL, 0x11379625747D5AF3ULL, 0xBCE5D2248682C115ULL,
  0x9DA4243DE836994FULL, 0x066F70B33FE09017ULL, 0x4DC4DE189B671A1CULL,
  0x51039AB7712457C3ULL, 0xC07A3F80C31FB4B4ULL, 0xB46EE9C5E64A6E7CULL,
  0xB3819A42ABE61C87ULL, 0x21A007933A522A20ULL, 0x2DF16F761598AA4FULL,
  0x763C4A1371B368FDULL, 0xF793C46702E086A0ULL, 0xD7288E012AEB8D31ULL,
  0xDE336A2A4BC1C44BULL, 0x0BF692B38D079F23ULL, 0x2C604A7A177326B3ULL,
  0x4850E73E03EB6064ULL, 0xCFC447F1E53C8E1BULL, 0xB05CA3F564268D99ULL,
  0x9AE182C8BC9474E8ULL, 0xA4FC4BD4FC5558CAULL, 0xE755178D58FC4E76ULL,
  0x69B97DB1A4C03DFEULL, 0xF9B5B7C4ACC67C96ULL, 0xFC6A82D64B8655FBULL,
  0x9C684CB6C4D24417ULL, 0x8EC97D2917456ED0ULL, 0x6703DF9D2924E97EULL,
  0xC547F57E42A7444EULL, 0x78E37644E7CAD29EULL, 0xFE9A44E9362F05FAULL,
  0x08BD35CC38336615ULL, 0x9315E5EB3A129ACEULL, 0x94061B871E04DF75ULL,
  0xDF1D9F9D784BA010ULL, 0x3BBA57B68871B59DULL, 0xD2B7ADEEDED1F73FULL,
  0xF7A255D83BC373F8ULL, 0xD7F4F2448C0CEB81ULL, 0xD95BE88CD210FFA7ULL,
  0x336F52F8FF4728E7ULL, 0xA74049DAC312AC71ULL, 0xA2F61BB6E437FDB5ULL,
  0x4F2A5CB07F6A35B3ULL, 0x87D380BDA5BF7859ULL, 0x16B9F7E06C453A21ULL,
  0x7BA2484C8A0FD54EULL, 0xF3A678CAD9A2E38CULL, 0x39B0BF7DDE437BA2ULL,
  0xFCAF55C1BF8A4424ULL, 0x18FCF680573FA594ULL, 0x4C0563B89F495AC3ULL,
  0x40E087931A00930DULL, 0x8CFFA9412EB642C1ULL, 0x68CA39053261169FULL,
  0x7A1EE967D27579E2ULL, 0x9D1D60E5076F5B6FULL, 0x3810E399B6F65BA2ULL,
  0x32095B6D4AB5F9B1ULL, 0x35CAB62109DD038AULL, 0xA90B24499FCFAFB1ULL,
  0x77A225A07CC2C6BDULL, 0x513E5E634C70E331ULL, 0x4361C0CA3F692F12ULL,
  0xD941ACA44B20A45BULL, 0x528F7C8602C5807BULL, 0x52AB92BEB9613989ULL,
  0x9D1DFA2EFC557F73ULL, 0x722FF175F572C348ULL, 0x1D1260A51107FE97ULL,
  0x7A249A57EC0C9BA2ULL, 0x04208FE9E8F7F2D6ULL, 0x5A110C6058B920A0ULL,
  0x0CD9A497658A5698ULL, 0x56FD23C8F9715A4CULL, 0x284C847B9D887AAEULL,
  0x04FEABFBBDB619CBULL, 0x742E1E651C60BA83ULL, 0x9A9632E65904AD3CULL,
  0x881B82A13B51B9E2ULL, 0x506E6744CD974924ULL, 0xB0183DB56FFC6A79ULL,
  0x0ED9B915C66ED37EULL, 0x5E11E86D5873D484ULL, 0xF678647E3519AC6EULL,
  0x1B85D488D0F20CC5ULL, 0xDAB9FE6525D89021ULL, 0x0D151D86ADB73615ULL,
  0xA865A54EDCC0F019ULL, 0x93C42566AEF98FFBULL, 0x99E7AFEABE000731ULL,
  0x48CBFF086DDF285AULL, 0x7F9B6AF1EBF78BAFULL, 0x58627E1A149BBA21ULL,
  0x2CD16E2ABD791E33ULL, 0xD363EFF5F0977996ULL, 0x0CE2A38C344A6EEDULL,
  0x1A804AADB9CFA741ULL, 0x907F30421D78C5DEULL, 0x501F65EDB3034D07ULL,
  0x37624AE5A48FA6E9ULL, 0x957BAF61700CFF4EULL, 0x3A6C27934E31188AULL,
  0xD49503536ABCA345ULL, 0x088E049589C432E0ULL, 0xF943AEE7FEBF21B8ULL,
  0x6C3B8E3E336139D3ULL, 0x364F6FFA464EE52EULL, 0xD60F6DCEDC314222ULL,
  0x56963B0DCA418FC0ULL, 0x16F50EDF91E513AFULL, 0xEF1955914B609F93ULL,
  0x565601C0364E3228ULL, 0xECB53939887E8175ULL, 0xBAC7A9A18531294BULL,
  0xb344c470397bba52ULL, 0x65d34954daf3cebdULL, 0xb4b81b3fa97511e2ULL,
  0xb422061193d6f6a7ULL, 0x071582401c38434dULL, 0x7a13f18bbedc4ff5ULL,
  0xbc4097b116c524d2ULL, 0x59b97885e2f2ea28ULL, 0x99170a5dc3115544ULL,
  0x6f423357e7c6a9f9ULL, 0x325928ee6e6f8794ULL, 0xd0e4366228b03343ULL,
  0x565c31f7de89ea27ULL, 0x30f5611484119414ULL, 0xd873db391292ed4fULL,
  0x7bd94e1d8e17debcULL, 0xc7d9f16864a76e94ULL, 0x947ae053ee56e63cULL,
  0xc8c93882f9475f5fULL, 0x3a9bf55ba91f81caULL, 0xd9a11fbb3d9808e4ULL,
  0x0fd22063edc29fcaULL, 0xb3f256d8aca0b0b9ULL, 0xb03031a8b4516e84ULL,
  0x35dd37d5871448afULL, 0xe9f6082b05542e4eULL, 0xebfafa33d7254b59ULL,
  0x9255abb50d532280ULL, 0xb9ab4ce57f2d34f3ULL, 0x693501d628297551ULL,
  0xc62c58f97dd949bfULL, 0xcd454f8f19c5126aULL, 0xbbe83f4ecc2bdecbULL,
  0xdc842b7e2819e230ULL, 0xba89142e007503b8ULL, 0xa3bc941d0a5061cbULL,
  0xe9f6760e32cd8021ULL, 0x09c7e552bc76492fULL, 0x852f54934da55cc9ULL,
  0x8107fccf064fcf56ULL, 0x098954d51fff6580ULL, 0x23b70edb1955c4bfULL,
  0xc330de426430f69dULL, 0x4715ed43e8a45c0aULL, 0xa8d7e4dab780a08dULL,
  0x0572b974f03ce0bbULL, 0xb57d2e985e1419c7ULL, 0xe8d9ecbe2cf3d73fULL,
  0x2fe4b17170e59750ULL, 0x11317ba87905e790ULL, 0x7fbf21ec8a1f45ecULL,
  0x1725cabfcb045b00ULL, 0x964e915cd5e2b207ULL, 0x3e2b8bcbf016d66dULL,
  0xbe7444e39328a0acULL, 0xf85b2b4fbcde44b7ULL, 0x49353fea39ba63b1ULL,
  0x1dd01aafcd53486aULL, 0x1fca8a92fd719f85ULL, 0xfc7c95d827357afaULL,
  0x18a6a990c8b35ebdULL, 0xcccb7005c6b9c28dULL, 0x3bdbb92c43b17f26ULL,
  0xaa70b5b4f89695a2ULL, 0xe94c39a54a98307fULL, 0xb7a0b174cff6f36eULL,
  0xd4dba84729af48adULL, 0x2e18bc1ad9704a68ULL, 0x2de0966daf2f8b1cULL,
  0xb9c11d5b1e43a07eULL, 0x64972d68dee33360ULL, 0x94628d38d0c20584ULL,
  0xdbc0d2b6ab90a559ULL, 0xd2733c4335c6a72fULL, 0x7e75d99d94a70f4dULL,
  0x6ced1983376fa72bULL, 0x97fcaacbf030bc24ULL, 0x7b77497b32503b12ULL,
  0x8547eddfb81ccb94ULL, 0x79999cdff70902cbULL, 0xcffe1939438e9b24ULL,
  0x829626e3892d95d7ULL, 0x92fae24291f2b3f1ULL, 0x63e22c147b9c3403ULL,
  0xc678b6d860284a1cULL, 0x5873888850659ae7ULL, 0x0981dcd296a8736dULL,
  0x9f65789a6509a440ULL, 0x9ff38fed72e9052fULL, 0xe479ee5b9930578cULL,
  0xe7f28ecd2d49eecdULL, 0x56c074a581ea17feULL, 0x5544f7d774b14aefULL,
  0x7b3f0195fc6f290fULL, 0x12153635b2c0cf57ULL, 0x7f5126dbba5e0ca7ULL,
  0x7a76956c3eafb413ULL, 0x3d5774a11d31ab39ULL, 0x8a1b083821f40cb4ULL,
  0x7b4a38e32537df62ULL, 0x950113646d1d6e03ULL, 0x4da8979a0041e8a9ULL,
  0x3bc36e078f7515d7ULL, 0x5d0a12f27ad310d1ULL, 0x7f9d1a2e1ebe1327ULL,
  0xda3a361b1c5157b1ULL, 0xdcdd7d20903d0c25ULL, 0x36833336d068f707ULL,
  0xce68341f79893389ULL, 0xab9090168dd05f34ULL, 0x43954b3252dc25e5ULL,
  0xb438c2b67f98e5e9ULL, 0x10dcd78e3851a492ULL, 0xdbc27ab5447822bfULL,
  0x9b3cdb65f82ca382ULL, 0xb67b7896167b4c84ULL, 0xbfced1b0048eac50ULL,
  0xa9119b60369ffebdULL, 0x1fff7ac80904bf45ULL, 0xac12fb171817eee7ULL,
  0xaf08da9177dda93dULL, 0x1b0cab936e65c744ULL, 0xb559eb1d04e5e932ULL,
  0xc37b45b3f8d6f2baULL, 0xc3a9dc228caac9e9ULL, 0xf3b8b6675a6507ffULL,
  0x9fc477de4ed681daULL, 0x67378d8eccef96cbULL, 0x6dd856d94d259236ULL,
  0xa319ce15b0b4db31ULL, 0x073973751f12dd5eULL, 0x8a8e849eb32781a5ULL,
  0xe1925c71285279f5ULL, 0x74c04bf1790c0efeULL, 0x4dda48153c94938aULL,
  0x9d266d6a1cc0542cULL, 0x7440fb816508c4feULL, 0x13328503df48229fULL,
  0xd6bf7baee43cac40ULL, 0x4838d65f6ef6748fULL, 0x1e152328f3318deaULL,
  0x8f8419a348f296bfULL, 0x72c8834a5957b511ULL, 0xd7a023a73260b45cULL,
  0x94ebc8abcfb56daeULL, 0x9fc10d0f989993e0ULL, 0xde68a2355b93cae6ULL,
  0xa44cfe79ae538bbeULL, 0x9d1d84fcce371425ULL, 0x51d2b1ab2ddfb636ULL,
  0x2fd7e4b9e72cd38cULL, 0x65ca5b96b7552210ULL, 0xdd69a0d8ab3b546dULL,
  0x604d51b25fbf70e2ULL, 0x73aa8a564fb7ac9eULL, 0x1a8c1e992b941148ULL,
  0xaac40a2703d9bea0ULL, 0x764dbeae7fa4f3a6ULL, 0x1e99b96e70a9be8bULL,
  0x2c5e9deb57ef4743ULL, 0x3a938fee32d29981ULL, 0x26e6db8ffdf5adfeULL,
  0x469356c504ec9f9dULL, 0xc8763c5b08d1908cULL, 0x3f6c6af859d80055ULL,
  0x7f7cc39420a3a545ULL, 0x9bfb227ebdf4c5ceULL, 0x89039d79d6fc5c5cULL,
  0x8fe88b57305e2ab6ULL, 0xa09e8c8c35ab96deULL, 0xfa7e393983325753ULL,
  0xd6b6d0ecc617c699ULL, 0xdfea21ea9e7557e3ULL, 0xb67c1fa481680af8ULL,
  0xca1e3785a9e724e5ULL, 0x1cfc8bed0d681639ULL, 0xd18d8549d140caeaULL,
  0x4ed0fe7e9dc91335ULL, 0xe4dbf0634473f5d2ULL, 0x1761f93a44d5aefeULL,
  0x53898e4c3910da55ULL, 0x734de8181f6ec39aULL, 0x2680b122baa28d97ULL,
  0x298af231c85bafabULL, 0x7983eed3740847d5ULL, 0x66c1a2a1a60cd889ULL,
  0x9e17e49642a3e4c1ULL, 0xedb454e7badc0805ULL, 0x50b704cab602c329ULL,
  0x4cc317fb9cddd023ULL, 0x66b4835d9eafea22ULL, 0x219b97e26ffc81bdULL,
  0x261e4e4c0a333a9dULL, 0x1fe2cca76517db90ULL, 0xd7504dfa8816edbbULL,
  0xb9571fa04dc089c8ULL, 0x1ddc0325259b27deULL, 0xcf3f4688801eb9aaULL,
  0xf4f5d05c10cab243ULL, 0x38b6525c21a42b0eULL, 0x36f60e2ba4fa6800ULL,
  0xeb3593803173e0ceULL, 0x9c4cd6257c5a3603ULL, 0xaf0c317d32adaa8aULL,
  0x258e5a80c7204c4bULL, 0x8b889d624d44885dULL, 0xf4d14597e660f855ULL,
  0xd4347f66ec8941c3ULL, 0xe699ed85b0dfb40dULL, 0x2472f6207c2d0484ULL,
  0xc2a1e7b5b459aeb5ULL, 0xab4f6451cc1d45ecULL, 0x63767572ae3d6174ULL,
  0xa59e0bd101731a28ULL, 0x116d0016cb948f09ULL, 0x2cf9c8ca052f6e9fULL,
  0x0b090a7560a968e3ULL, 0xabeeddb2dde06ff1ULL, 0x58efc10b06a2068dULL,
  0xc6e57a78fbd986e0ULL, 0x2eab8ca63ce802d7ULL, 0x14a195640116f336ULL,
  0x7c0828dd624ec390ULL, 0xd74bbe77e6116ac7ULL, 0x804456af10f5fb53ULL,
  0xebe9ea2adf4321c7ULL, 0x03219a39ee587a30ULL, 0x49787fef17af9924ULL,
  0xa1e9300cd8520548ULL, 0x5b45e522e4b1b4efULL, 0xb49c3b3995091a36ULL,
  0xd4490ad526f14431ULL, 0x12a8f216af9418c2ULL, 0x001f837cc7350524ULL,
  0x1877b51e57a764d5ULL, 0xa2853b80f17f58eeULL, 0x993e1de72d36d310ULL,
  0xb3598080ce64a656ULL, 0x252f59cf0d9f04bbULL, 0xd23c8e176d113600ULL,
  0x1bda0492e7e4586eULL, 0x21e0bd5026c619bfULL, 0x3b097adaf088f94eULL,
  0x8d14dedb30be846eULL, 0xf95cffa23af5f6f4ULL, 0x3871700761b3f743ULL,
  0xca672b91e9e4fa16ULL, 0x64c8e531bff53b55ULL, 0x241260ed4ad1e87dULL,
  0x106c09b972d2e822ULL, 0x7fba195410e5ca30ULL, 0x7884d9bc6cb569d8ULL,
  0x0647dfedcd894a29ULL, 0x63573ff03e224774ULL, 0x4fc8e9560f91b123ULL,
  0x1db956e450275779ULL, 0xb8d91274b9e9d4fbULL, 0xa2ebee47e2fbfce1ULL,
  0xd9f1f30ccd97fb09ULL, 0xefed53d75fd64e6bULL, 0x2e6d02c36017f67fULL,
  0xa9aa4d20db084e9bULL, 0xb64be8d8b25396c1ULL, 0x70cb6af7c2d5bcf0ULL,
  0x98f076a4f7a2322eULL, 0xbf84470805e69b5fULL, 0x94c3251f06f90cf3ULL,
  0x3e003e616a6591e9ULL, 0xb925a6cd0421aff3ULL, 0x61bdd1307c66e300ULL,
  0xbf8d5108e27e0d48ULL, 0x240ab57a8b888b20ULL, 0xfc87614baf287e07ULL,
  0xef02cdd06ffdb432ULL, 0xa1082c0466df6c0aULL, 0x8215e577001332c8ULL,
  0xd39bb9c3a48db6cfULL, 0x2738259634305c14ULL, 0x61cf4f94c97df93dULL,
  0x1b6baca2ae4e125bULL, 0x758f450c88572e0bULL, 0x959f587d507a8359ULL,
  0xb063e962e045f54dULL, 0x60e8ed72c0dff5d1ULL, 0x7b64978555326f9fULL,
  0xfd080d236da814baULL, 0x8c90fd9b083f4558ULL, 0x106f72fe81e2c590ULL,
  0x7976033a39f7d952ULL, 0xa4ec0132764ca04bULL, 0x733ea705fae4fa77ULL,
  0xb4d8f77bc3e56167ULL, 0x9e21f4f903b33fd9ULL, 0x9d765e419fb69f6dULL,
  0xd30c088ba61ea5efULL, 0x5d94337fbfaf7f5bULL, 0x1a4e4822eb4d7a59ULL,
  0x6ffe73e81b637fb3ULL, 0xddf957bc36d8b9caULL, 0x64d0e29eea8838b3ULL,
  0x08dd9bdfd96b9f63ULL, 0x087e79e5a57d1d13ULL, 0xe328e230e3e2b3fbULL,
  0x1c2559e30f0946beULL, 0x720bf5f26f4d2eaaULL, 0xb0774d261cc609dbULL,
  0x443f64ec5a371195ULL, 0x4112cf68649a260eULL, 0xd813f2fab7f5c5caULL,
  0x660d3257380841eeULL, 0x59ac2c7873f910a3ULL, 0xe846963877671a17ULL,
  0x93b633abfa3469f8ULL, 0xc0c0f5a60ef4cdcfULL, 0xcaf21ecd4377b28cULL,
  0x57277707199b8175ULL, 0x506c11b9d90e8b1dULL, 0xd83cc2687a19255fULL,
  0x4a29c6465a314cd1ULL, 0xed2df21216235097ULL, 0xb5635c95ff7296e2ULL,
  0x22af003ab672e811ULL, 0x52e762596bf68235ULL, 0x9aeba33ac6ecc6b0ULL,
  0x944f6de09134dfb6ULL, 0x6c47bec883a7de39ULL, 0x6ad047c430a12104ULL,
  0xa5b1cfdba0ab4067ULL, 0x7c45d833aff07862ULL, 0x5092ef950a16da0bULL,
  0x9338e69c052b8e7bULL, 0x455a4b4cfe30e3f5ULL, 0x6b02e63195ad0cf8ULL,
  0x6b17b224bad6bf27ULL, 0xd1e0ccd25bb9c169ULL, 0xde0c89a556b9ae70ULL,
  0x50065e535a213cf6ULL, 0x9c1169fa2777b874ULL, 0x78edefd694af1eedULL,
  0x6dc93d9526a50e68ULL, 0xee97f453f06791edULL, 0x32ab0edb696703d3ULL,
  0x3a6853c7e70757a7ULL, 0x31865ced6120f37dULL, 0x67fef95d92607890ULL,
  0x1f2b1d1f15f6dc9cULL, 0xb69e38a8965c6b65ULL, 0xaa9119ff184cccf4ULL,
  0xf43c732873f24c13ULL, 0xfb4a3d794a9a80d2ULL, 0x3550c2321fd6109cULL,
  0x371f77e76bb8417eULL, 0x6bfa9aae5ec05779ULL, 0xcd04f3ff001a4778ULL,
  0xe3273522064480caULL, 0x9f91508bffcfc14aULL, 0x049a7f41061a9e60ULL,
  0xfcb6be43a9f2fe9bULL, 0x08de8a1c7797da9bULL, 0x8f9887e6078735a1ULL,
  0xb5b4071dbfc73a66ULL, 0x230e343dfba08d33ULL, 0x43ed7f5a0fae657dULL,
  0x3a88a0fbbc605c63ULL, 0x21874b8b4d2dbc4fULL, 0x1bdea12e35f6a8c9ULL,
  0x53c065c6c8e63528ULL, 0xe34a1d250e7a8d6bULL, 0xd6b04d3b7651dd7eULL,
  0x5e90277e7cb39e2dULL, 0x2c046f22062dc67dULL, 0xb10bb459132d0a26ULL,
  0x3fa9ddfb67e2f199ULL, 0x0e09b88e1914f7afULL, 0x10e8b35af3eeab37ULL,
  0x9eedeca8e272b933ULL, 0xd4c718bc4ae8ae5fULL, 0x81536d601170fc20ULL,
  0x91b534f885818a06ULL, 0xec8177f83f900978ULL, 0x190e714fada5156eULL,
  0xb592bf39b0364963ULL, 0x89c350c893ae7dc1ULL, 0xac042e70f8b383f2ULL,
  0xb49b52e587a1ee60ULL, 0xfb152fe3ff26da89ULL, 0x3e666e6f69ae2c15ULL,
  0x3b544ebe544c19f9ULL, 0xe805a1e290cf2456ULL, 0x24b33c9d7ed25117ULL,
  0xe74733427b72f0c1ULL, 0x0a804d18b7097475ULL, 0x57e3306d881edb4fULL,
  0x4ae7d6a36eb5dbcbULL, 0x2d8d5432157064c8ULL, 0xd1e649de1e7f268bULL,
  0x8a328a1cedfe552cULL, 0x07a3aec79624c7daULL, 0x84547ddc3e203c94ULL,
  0x990a98fd5071d263ULL, 0x1a4ff12616eefc89ULL, 0xf6f7fd1431714200ULL,
  0x30c05b1ba332f41cULL, 0x8d2636b81555a786ULL, 0x46c9feb55d120902ULL,
  0xccec0a73b49c9921ULL, 0x4e9d2827355fc492ULL, 0x19ebb029435dcb0fULL,
  0x4659d2b743848a2cULL, 0x963ef2c96b33be31ULL, 0x74f85198b05a2e7dULL,
  0x5a0f544dd2b1fb18ULL, 0x03727073c2e134b1ULL, 0xc7f6aa2de59aea61ULL,
  0x352787baa0d7c22fULL, 0x9853eab63b5e0b35ULL, 0xabbdcdd7ed5c0860ULL,
  0xcf05daf5ac8d77b0ULL, 0x49cad48cebf4a71eULL, 0x7a4c10ec2158c4a6ULL,
  0xd9e92aa246bf719eULL, 0x13ae978d09fe5557ULL, 0x730499af921549ffULL,
  0x4e4b705b92903ba4ULL, 0xff577222c14f0a3aULL, 0x55b6344cf97aafaeULL,
  0xb862225b055b6960ULL, 0xcac09afbddd2cdb4ULL, 0xdaf8e9829fe96b5fULL,
  0xb5fdfc5d3132c498ULL, 0x310cb380db6f7503ULL, 0xe87fbb46217a360eULL,
  0x2102ae466ebb1148ULL, 0xf8549e1a3aa5e00dULL, 0x07a69afdcc42261aULL,
  0xc4c118bfe78feaaeULL, 0xf9f4892ed96bd438ULL, 0x1af3dbe25d8f45daULL,
  0xf5b4b0b0d2deeeb4ULL, 0x962aceefa82e1c84ULL, 0x046e3ecaaf453ce9ULL,
  0xf05d129681949a4cULL, 0x964781ce734b3c84ULL, 0x9c2ed44081ce5fbdULL,
  0x522e23f3925e319eULL, 0x177e00f9fc32f791ULL, 0x2bc60a63a6f3b3f2ULL,
  0x222bbfae61725606ULL, 0x486289ddcc3d6780ULL, 0x7dc7785b8efdfc80ULL,
  0x8af38731c02ba980ULL, 0x1fab64ea29a2ddf7ULL, 0xe4d9429322cd065aULL,
  0x9da058c67844f20cULL, 0x24c0e332b70019b0ULL, 0x233003b5a6cfe6adULL,
  0xd586bd01c5c217f6ULL, 0x5e5637885f29bc2bULL, 0x7eba726d8c94094bULL,
  0x0a56a5f0bfe39272ULL, 0xd79476a84ee20d06ULL, 0x9e4c1269baa4bf37ULL,
  0x17efee45b0dee640ULL, 0x1d95b0a5fcf90bc6ULL, 0x93cbe0b699c2585dULL,
  0x65fa4f227a2b6d79ULL, 0xd5f9e858292504d5ULL, 0xc2b5a03f71471a6fULL,
  0x59300222b4561e00ULL, 0xce2f8642ca0712dcULL, 0x7ca9723fbb2e8988ULL,
  0x2785338347f2ba08ULL, 0xc61bb3a141e50e8cULL, 0x150f361dab9dec26ULL,
  0x9f6a419d382595f4ULL, 0x64a53dc924fe7ac9ULL, 0x142de49fff7a7c3dULL,
  0x0c335248857fa9e7ULL, 0x0a9c32d5eae45305ULL, 0xe6c42178c4bbb92eULL,
  0x71f1ce2490d20b07ULL, 0xf1bcc3d275afe51aULL, 0xe728e8c83c334074ULL,
  0x96fbf83a12884624ULL, 0x81a1549fd6573da5ULL, 0x5fa7867caf35e149ULL,
  0x56986e2ef3ed091bULL, 0x917f1dd5f8886c61ULL, 0xd20d8c88c8ffe65fULL,
  0x31d71dce64b2c310ULL, 0xf165b587df898190ULL, 0xa57e6339dd2cf3a0ULL,
  0x1ef6e6dbb1961ec9ULL, 0x70cc73d90bc26e24ULL, 0xe21a6b35df0c3ad7ULL,
  0x003a93d8b2806962ULL, 0x1c99ded33cb890a1ULL, 0xcf3145de0add4289ULL,
  0xd0e4427a5514fb72ULL, 0x77c621cc9fb3a483ULL, 0x67a34dac4356550bULL,
  0xf8d626aaaf278509ULL
} };

PolyBook polybook, polybook2;

static bool useBestBookMove = true;
static int maxBookDepth = 255;
PRNG sr;

static bool initialised = false;

static void pb_release(PolyBook *pb)
{
  if (pb->polyhash) {
#ifndef USE_EMBEDDED_BOOK
    unmap_file(pb->polyhash, pb->mapping);
#endif
    pb->polyhash = NULL;
  }
}

void pb_free(void)
{
  pb_release(&polybook);
  pb_release(&polybook2);
}

void pb_init(PolyBook *pb, const char *bookfile)
{
  if (!initialised) {
    prng_init(&sr, time(NULL));
    for (int i = 0; i < 10; i++)
      prng_rand(&sr);
    initialised = true;
  }

  // If set to empty, disable and release memory
  if (!bookfile || strlen(bookfile) == 0 || strcmp(bookfile, "<empty>") == 0) {
    pb_release(pb);
    pb->enabled = false;
    return;
  }

  pb_release(pb);

#ifdef USE_EMBEDDED_BOOK
  // Check if we want to use the embedded Best.bin file
  if (strcmp(bookfile, "<embedded>") == 0) {
    size_t book_size = (size_t)_binary_Best_bin_size;
    
    // Safety check to ensure symbols are correctly linked
    if (book_size == 0 || _binary_Best_bin_start == NULL) {
      pb->enabled = false;
      return;
    }

    pb->keycount = book_size / sizeof(struct PolyHash);
    pb->polyhash = (const struct PolyHash *)_binary_Best_bin_start;

    pb->enabled = true;
    pb->do_search = true;
    return;
  }
#endif

  // Fallback / Standard: Load external file from SD card
  FD fd = open_file(bookfile);
  if (fd != FD_ERR) {
    pb->keycount = file_size(fd) / 16;
    pb->polyhash = map_file(fd, &pb->mapping);
    close_file(fd);
  }

  if (!pb->polyhash) {
    printf("info string Could not open %s\n", bookfile);
    pb->enabled = false;
    return;
  }

  printf("info string Book loaded: %s\n", bookfile);
  pb->enabled = true;
  pb->do_search = true;
}

void pb_set_best_book_move(bool best_book_move)
{
  useBestBookMove = best_book_move;
}

void pb_set_book_depth(int book_depth)
{
  maxBookDepth = book_depth;
}

Move pb_probe(PolyBook *pb, Position *pos)
{
  Move m1 = 0;

  if (!pb->enabled) return m1;
  if (!check_do_search(pb, pos)) return m1;

  if (pb->book_depth_count >= maxBookDepth)
    return m1;

  Key key = polyglot_key(pos);

  int n = find_first_key(pb, key);

  if (n < 1) {
    pb->search_counter++;
    if (pb->search_counter > 4) {
      pb->do_search = false;
      pb->search_counter = 0;
      pb->book_depth_count = 0;
    }

    return m1;
  }

  pb->book_depth_count++;

  ssize_t idx1 = useBestBookMove ? pb->index_best : pb->index_rand;

  // Uses alignment-safe readers to fetch values safely
  m1 = pg_move_to_sf_move(pos, read_move(&pb->polyhash[idx1]));

  if (!is_draw(pos)) return m1; // 64
  if (n == 1) return m1;

  // Special case draw position and 2 moves available

  if (!check_draw(pos, m1))
    return m1;

  ssize_t idx2 = pb->index_first;
  if (idx1 == idx2) idx2++;

  // Uses alignment-safe readers to fetch values safely
  Move m2 = pg_move_to_sf_move(pos, read_move(&pb->polyhash[idx2]));

  if (!check_draw(pos, m2))
    return m2;

  return 0;
}

static Key polyglot_key(const Position *pos)
{
  Key key = 0;
  Bitboard b = pieces();

  while (b) {
    Square s = pop_lsb(&b);
    Piece p = piece_on(s);

    // PolyGlot pieces are: BP = 0, WP = 1, BN = 2, ... BK = 10, WK = 11
    key ^= PG.Zobrist.psq[2 * (type_of_p(p) - 1) + (color_of(p) == WHITE)][s];
  }

  b = can_castle_any();

  while (b)
    key ^= PG.Zobrist.castle[pop_lsb(&b)];

  if (ep_square())
    key ^= PG.Zobrist.enpassant[file_of(ep_square())];

  if (stm() == WHITE)
    key ^= PG.Zobrist.turn;

  return key;
}

static Move pg_move_to_sf_move(const Position *pos, uint16_t pg_move)
{
  Move move = (Move)pg_move;

  int pt = (move >> 12) & 7;
  if (pt)
    return make_promotion(from_sq(move), to_sq(move), pt + 1);

  // Add 'special move' flags and verify it is legal
  ExtMove *m = (pos->st-1)->endMoves;
  ExtMove *end = generate_legal(pos, m);
  for (; m < end; m++)
    if (move == (m->move & (~(3 << 14)))) //  Compare with MoveType (bit 14-15)  masked out
      return m->move;

  return 0;
}

static int find_first_key(PolyBook *pb, uint64_t key)
{
  pb->index_first = -1;
  pb->index_count = 0;
  pb->index_weight_count = 0;
  pb->index_best = -1;
  pb->index_rand = -1;

  ssize_t start = 0;
  ssize_t end = pb->keycount;

  do {
    ssize_t mid = (end + start) / 2;

    // Uses alignment-safe readers to evaluate keys safely
    if (read_key(&pb->polyhash[mid]) < key)
      start = mid;
    else {
      if (read_key(&pb->polyhash[mid]) > key)
        end = mid;
      else {
        start = max(mid - 4, 0);
        end = min(mid + 4, pb->keycount);
      }
    }
  } while (end - start > 8);

  for (ssize_t i = start; i < end; i++)
    if (key == read_key(&pb->polyhash[i])) {
      pb->index_first = i;
      while (   pb->index_first > 0
             && key == read_key(&pb->polyhash[pb->index_first - 1]))
        pb->index_first--;
      return get_key_data(pb);
    }

  return -1;
}

static int get_key_data(PolyBook *pb)
{
  // Uses alignment-safe readers to evaluate keys and weights safely
  int best_weight = read_weight(&pb->polyhash[pb->index_first]);
  pb->index_weight_count = best_weight;
  uint64_t key = read_raw_key(&pb->polyhash[pb->index_first]);

  pb->index_count = 1;
  pb->index_best = pb->index_first;

  for (ssize_t i = pb->index_first + 1; i < pb->keycount; i++) {
    if (read_raw_key(&pb->polyhash[i]) != key)
      break;

    pb->index_count++;
    pb->index_weight_count += read_weight(&pb->polyhash[i]);
    if (read_weight(&pb->polyhash[i]) > best_weight) {
      best_weight = read_weight(&pb->polyhash[i]);
      pb->index_best = i;
    }
  }

  int rand_pos = prng_rand(&sr) % pb->index_weight_count;
  int weight_count = 0;
  pb->index_rand = pb->index_best;

  for (ssize_t i = pb->index_first; i < pb->index_first + pb->index_count; i++) {
    if (   rand_pos >= weight_count
        && rand_pos < weight_count + read_weight(&pb->polyhash[i]))
    {
      pb->index_rand = i;
      break;
    }
    weight_count += read_weight(&pb->polyhash[i]);
  }

  return pb->index_count;
}

static bool check_do_search(PolyBook *pb, const Position *pos)
{
  pb->akt_position = pieces();
  pb->akt_anz_pieces = popcount(pb->akt_position);

  Bitboard b = pb->akt_position ^ pb->last_position;
  int n2 = popcount(b);

  bool pos_changed =   n2 > 6
                    || pb->akt_position == pb->last_position
                    || pb->akt_anz_pieces > pb->last_anz_pieces
                    || pb->akt_anz_pieces < pb->last_anz_pieces - 2
                    || raw_key() == 0xB4D30CD15A43432D;

  if (pos_changed) {
    pb->book_depth_count = 0;
    pb->do_search = true;
  }

  pb->last_position = pb->akt_position;
  pb->last_anz_pieces = pb->akt_anz_pieces;

  return pb->do_search;
}

static bool check_draw(Position *pos, Move m)
{
  do_move(pos, m, gives_check(pos, pos->st, m));
  bool draw = is_draw(pos); // 64
  undo_move(pos, m);

  return draw;
}
