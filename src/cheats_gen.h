#pragma once
#include <vector>
#include <string>
#include <cstdint>

// Aggregate used by the auto-generated populateCheats().
struct GenPatch {
    uint32_t off;
    std::vector<uint8_t> patch;
    std::vector<uint8_t> orig;
};

// Implemented in dllmain.cpp; called by the generated cheats_generated.cpp.
void addPatch(const char* cat, const char* name, std::vector<GenPatch> patches);
void addCave(const char* cat, const char* name, uint32_t hookOff, int slot,
             std::vector<uint8_t> orig, std::vector<uint8_t> tmpl,
             uint32_t B0, std::vector<int> rel, int size);

// Generated.
void populateCheats();
void populateCaves();
