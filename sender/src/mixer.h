#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Pos {
    double x;
    double y;
};

enum class Role { FL, FR, FC, LFE, BL, BR, FLC, FRC, BC, SL, SR, None };

struct Layout {
    int srcCh = 0;
    std::vector<Role> roles;
};

Layout makeLayout(int srcCh, uint32_t mask);
std::vector<float> stereoRows(const Layout& l);
std::vector<float> surroundRows(const Layout& l, const std::vector<Pos>& phones);
void mixBlock(const int16_t* in, int frames, int srcCh, const float* rows, int outCh, int16_t* out);
double azimuthDeg(Pos p);
std::string roleName(Pos p);
