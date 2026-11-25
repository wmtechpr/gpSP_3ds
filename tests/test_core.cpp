#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "gpsp_bridge.h"
#include "3dsaudio_ring.h"
#include "3dscheat_parse.h"

static void putLe32(std::vector<uint8_t>& buf, size_t off, uint32_t v) {
    buf[off + 0] = static_cast<uint8_t>(v & 0xFF);
    buf[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    buf[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    buf[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

static std::vector<uint8_t> buildSyntheticGbaRom() {
    std::vector<uint8_t> rom(32 * 1024, 0x00);

    // ARM reset vector at 0x000: b 0x080000C0
    putLe32(rom, 0x000, 0xEA00002Eu);

    // Header title (0x0A0..0x0AB) & game code (0x0AC..0x0AF)
    memcpy(&rom[0x0A0], "GPSP3DSTEST", 11);
    memcpy(&rom[0x0AC], "G3DT", 4);
    rom[0x0B0] = '0';
    rom[0x0B1] = '1';
    rom[0x0B2] = 0x96;

    // Complement check at 0x0BD
    int sum = 0;
    for (int i = 0x0A0; i <= 0x0BC; i++) {
        sum += rom[i];
    }
    rom[0x0BD] = static_cast<uint8_t>(-(sum + 0x19) & 0xFF);

    // ARM entry code at 0x0C0:
    // Enable Mode 3 + BG2 (DISPCNT=0x0403), enable GBA PSG Ch1 sound,
    // write BGR555 pure red (0x001F -> RGB565 0xF800) to first pixels of Mode 3 VRAM, and loop.
    static const uint32_t prog[] = {
        0xE3A00404u, // mov  r0, #0x04000000
        0xE3A01B01u, // mov  r1, #0x0400
        0xE3811003u, // orr  r1, r1, #0x03
        0xE1C010B0u, // strh r1, [r0, #0]     ; DISPCNT = 0x0403 (Mode 3 + BG2)
        0xE3A01080u, // mov  r1, #0x80
        0xE1C018B4u, // strh r1, [r0, #0x84]  ; SOUNDCNT_X = 0x80 (Master on)
        0xE3A01CFFu, // mov  r1, #0xFF00
        0xE3811077u, // orr  r1, r1, #0x77
        0xE1C018B0u, // strh r1, [r0, #0x80]  ; SOUNDCNT_L = 0xFF77
        0xE3A01002u, // mov  r1, #0x02
        0xE1C018B2u, // strh r1, [r0, #0x82]  ; SOUNDCNT_H = 0x0002 (100% PSG)
        0xE3A01A0Fu, // mov  r1, #0xF000
        0xE3811080u, // orr  r1, r1, #0x80
        0xE1C016B2u, // strh r1, [r0, #0x62]  ; SOUND1CNT_H = 0xF080
        0xE3A01B21u, // mov  r1, #0x8400
        0xE1C016B4u, // strh r1, [r0, #0x64]  ; SOUND1CNT_X = 0x8400 (trigger Ch1)
        0xE3A00406u, // mov  r0, #0x06000000  ; VRAM
        0xE3A0101Fu, // mov  r1, #0x001F      ; BGR555 red -> RGB565 0xF800
        0xE1C010B0u, // strh r1, [r0, #0]
        0xE1C010B2u, // strh r1, [r0, #2]
        0xE1C010B4u, // strh r1, [r0, #4]
        0xEAFFFFFEu  // b .
    };
    for (size_t i = 0; i < sizeof(prog) / sizeof(prog[0]); i++) {
        putLe32(rom, 0x0C0 + i * 4, prog[i]);
    }

    return rom;
}

static void testGpspCoreVideoAudioAndState() {
    const char* romPath = "/tmp/gpsp_test_rom.gba";
    std::vector<uint8_t> rom = buildSyntheticGbaRom();
    FILE* fp = fopen(romPath, "wb");
    assert(fp != nullptr);
    assert(fwrite(rom.data(), 1, rom.size(), fp) == rom.size());
    fclose(fp);

    assert(gpsp_bridge_init());

    GpspRomMeta meta = {};
    assert(gpsp_bridge_load_rom(romPath, nullptr, false, false, &meta));
    assert(strcmp(meta.title, "GPSP3DSTEST") == 0);
    assert(strcmp(meta.gameCode, "G3DT") == 0);
    assert(!meta.usingOfficialBios);
    assert(!meta.dynarecActive);

    std::vector<int16_t> audioBuf(2048 * 2, 0);
    size_t totalAudioFrames = 0;

    for (int f = 0; f < 15; f++) {
        gpsp_bridge_set_input(0);
        size_t gotFrames = 0;
        gpsp_bridge_run_frame(false, audioBuf.data(), 2048, &gotFrames);
        totalAudioFrames += gotFrames;
    }

    // Check rendered Mode 3 pixels (BGR555 0x001F -> RGB565 0xF800) and color correction LUT
    const uint16_t* fb = gpsp_bridge_get_framebuffer();
    assert(fb != nullptr);
    assert(fb[0] == 0xF800);
    assert(fb[1] == 0xF800);
    assert(fb[2] == 0xF800);
    assert(gpsp_bridge_apply_cc(0xF800) != 0);

    // 15 frames * ~548.625 samples/frame @ 32,768 Hz
    assert(totalAudioFrames > 7000 && totalAudioFrames < 9000);

    // Savestate round-trip
    size_t stCap = gpsp_bridge_state_size();
    assert(stCap == GPSP_STATE_MAX_SIZE);
    std::vector<uint8_t> state(stCap, 0);
    assert(gpsp_bridge_serialize(state.data(), stCap - 1) == 0);
    assert(gpsp_bridge_serialize(state.data(), stCap) == stCap);
    assert(!gpsp_bridge_unserialize(state.data(), stCap - 1));
    assert(gpsp_bridge_unserialize(state.data(), stCap));

    size_t postLoadAudio = 0;
    gpsp_bridge_run_frame(false, audioBuf.data(), 2048, &postLoadAudio);
    assert(postLoadAudio > 500 && postLoadAudio < 600);

    // CodeBreaker cheat parsing & clear
    assert(gpsp_bridge_cheat_add(0, "32000000 0042"));
    gpsp_bridge_cheats_clear();

    gpsp_bridge_finalize();
    unlink(romPath);

    printf("[PASS] testGpspCoreVideoAudioAndState (frames=15, audioSamples=%zu, stateSize=%zu)\n",
           totalAudioFrames, stCap);
}

static void testStereoRingBuffer() {
    StereoRingBuffer ring;
    int16_t tmp[StereoRingBuffer::CAPACITY_FRAMES * 2];

    memset(tmp, 0x5A, sizeof(int16_t) * 8);
    assert(ring.pop(tmp, 4) == 0);
    for (int i = 0; i < 8; i++) assert(tmp[i] == 0);

    int16_t inA[6] = { 10, -10, 20, -20, 30, -30 };
    assert(ring.push(inA, 3) == 0);
    assert(ring.available() == 3);
    assert(ring.pop(tmp, 2) == 2);
    assert(tmp[0] == 10 && tmp[1] == -10 && tmp[2] == 20 && tmp[3] == -20);

    std::vector<int16_t> chunk(5000 * 2);
    for (size_t i = 0; i < 5000; i++) {
        chunk[i * 2] = static_cast<int16_t>(i);
        chunk[i * 2 + 1] = static_cast<int16_t>(-static_cast<int>(i));
    }
    ring.reset();
    assert(ring.push(chunk.data(), 5000) == 0);
    assert(ring.pop(tmp, 4000) == 4000);
    assert(ring.push(chunk.data(), 5000) == 0);
    assert(ring.available() == 6000);
    assert(ring.pop(tmp, 1000) == 1000);
    assert(tmp[0] == 4000 && tmp[1] == -4000);

    ring.reset();
    assert(ring.push(chunk.data(), 5000) == 0);
    assert(ring.push(chunk.data(), 5000) == 1808);
    assert(ring.available() == StereoRingBuffer::CAPACITY_FRAMES);
    ring.reset();
    assert(ring.available() == 0);

    printf("[PASS] testStereoRingBuffer\n");
}

static void testCheatParserAndSerializer() {
    const char* rawCht =
        "cheats = 2\n"
        "cheat0_desc = \"Infinite HP\"\n"
        "cheat0_code = \"82000010 03E7\"\n"
        "cheat0_enable = true\n"
        "cheat1_desc = \"Max Coins\"\n"
        "cheat1_code = \"32000020 0063\"\n"
        "cheat1_enable = false\n";

    std::vector<ParsedCheat> parsed;
    parseChtText(rawCht, parsed, 256);
    assert(parsed.size() == 2);
    assert(parsed[0].name == "Infinite HP");
    assert(parsed[0].code == "82000010 03E7");
    assert(parsed[0].enabled == true);
    assert(parsed[1].enabled == false);

    std::string serialized = serializeChtText(parsed);
    std::vector<ParsedCheat> reparsed;
    parseChtText(serialized.c_str(), reparsed, 256);
    assert(reparsed.size() == 2);
    assert(reparsed[0].code == parsed[0].code);
    assert(reparsed[1].enabled == false);

    printf("[PASS] testCheatParserAndSerializer\n");
}

int main() {
    testGpspCoreVideoAudioAndState();
    testStereoRingBuffer();
    testCheatParserAndSerializer();
    printf("ALL HOST SELF-TESTS PASSED\n");
    return 0;
}
