#pragma once
#include "vec.h"
#include <string>
#include <vector>

struct Spring {
    int a, b;
    double rest, k;
};

struct Cloth {
    int n;
    std::vector<Vec3> x, xPrev, v, vPrev, y;
    std::vector<double> mass;
    std::vector<char> pinned;
    std::vector<Spring> springs;
    std::vector<std::vector<int>> springsOf;
    std::vector<std::vector<int>> colours;

    Cloth(int n, double size, double totalMass, double kStretch, double kBend);

    int size() const { return (int)x.size(); }
    void predict(int lo, int hi, double h);
    void solveVertex(int i, double h);
    void updateVelocity(int lo, int hi, double h, double damping);

    double maxStrain() const;
    double lowestY() const;
    void writeObj(const std::string& path) const;

private:
    int index(int i, int j) const { return i * n + j; }
    void connect(int i0, int j0, int i1, int j1, double k);
    void colourGraph();
};
