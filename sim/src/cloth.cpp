#include "cloth.h"
#include <algorithm>
#include <fstream>

Cloth::Cloth(int n, double size, double totalMass, double kStretch, double kBend) : n(n) {
    double spacing = size / (n - 1);
    x.resize(n * n);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) x[index(i, j)] = {j * spacing, 0, i * spacing};
    xPrev = y = x;
    v.assign(x.size(), {});
    vPrev = v;
    mass.assign(x.size(), totalMass / x.size());
    pinned.assign(x.size(), 0);
    pinned[index(0, 0)] = 1;

    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            connect(i, j, i, j + 1, kStretch);
            connect(i, j, i + 1, j, kStretch);
            connect(i, j, i + 1, j + 1, kStretch);
            connect(i, j, i + 1, j - 1, kStretch);
            connect(i, j, i, j + 2, kBend);
            connect(i, j, i + 2, j, kBend);
        }
    }

    springsOf.resize(x.size());
    for (int s = 0; s < (int)springs.size(); s++) {
        springsOf[springs[s].a].push_back(s);
        springsOf[springs[s].b].push_back(s);
    }

    colourGraph();
}

void Cloth::connect(int i0, int j0, int i1, int j1, double k) {
    if (i1 < 0 || i1 >= n || j1 < 0 || j1 >= n) return;
    int a = index(i0, j0), b = index(i1, j1);
    springs.push_back({a, b, norm(x[a] - x[b]), k});
}

void Cloth::colourGraph() {
    std::vector<int> colour(x.size(), -1);
    for (int i = 0; i < size(); i++) {
        std::vector<char> used(springsOf[i].size() + 1, 0);
        for (int s : springsOf[i]) {
            int j = springs[s].a == i ? springs[s].b : springs[s].a;
            if (colour[j] >= 0 && colour[j] < (int)used.size()) used[colour[j]] = 1;
        }
        int c = 0;
        while (used[c]) c++;
        colour[i] = c;
        if (pinned[i]) continue;
        if (c >= (int)colours.size()) colours.resize(c + 1);
        colours[c].push_back(i);
    }
}

void Cloth::predict(int lo, int hi, double h) {
    const Vec3 gravity{0, -9.81, 0};
    for (int i = lo; i < hi; i++) {
        xPrev[i] = x[i];
        if (pinned[i]) {
            y[i] = x[i];
            continue;
        }
        y[i] = x[i] + h * v[i] + (h * h) * gravity;

        // Adaptive initial guess (VBD paper): only as much gravity as the vertex actually felt last step.
        Vec3 accel = (1.0 / h) * (v[i] - vPrev[i]);
        double g = std::clamp(dot(accel, gravity) / dot(gravity, gravity), 0.0, 1.0);
        x[i] = x[i] + h * v[i] + (h * h * g) * gravity;
    }
}

// One Newton step on the vertex's local energy: m/(2h^2)|x - y|^2 + sum of spring energies.
void Cloth::solveVertex(int i, double h) {
    double inertia = mass[i] / (h * h);
    Vec3 force = inertia * (y[i] - x[i]);
    Mat3 hessian = identity(inertia);

    for (int s : springsOf[i]) {
        const Spring& sp = springs[s];
        int j = sp.a == i ? sp.b : sp.a;
        Vec3 d = x[i] - x[j];
        double len = norm(d);
        if (len < 1e-12) continue;
        Vec3 dir = (1.0 / len) * d;
        Mat3 nn = outer(dir, dir);

        force -= (sp.k * (len - sp.rest)) * dir;
        double lateral = std::max(0.0, 1.0 - sp.rest / len);
        hessian = hessian + sp.k * nn + (sp.k * lateral) * (identity(1) - nn);
    }

    x[i] += solve(hessian, force);
}

void Cloth::updateVelocity(int lo, int hi, double h, double damping) {
    double keep = std::max(0.0, 1.0 - damping * h);
    for (int i = lo; i < hi; i++) {
        vPrev[i] = v[i];
        v[i] = (keep / h) * (x[i] - xPrev[i]);
    }
}

double Cloth::maxStrain() const {
    double worst = 0;
    for (const Spring& sp : springs) worst = std::max(worst, std::abs(norm(x[sp.a] - x[sp.b]) - sp.rest) / sp.rest);
    return worst;
}

double Cloth::lowestY() const {
    double low = 0;
    for (const Vec3& p : x) low = std::min(low, p.y);
    return low;
}

void Cloth::writeObj(const std::string& path) const {
    std::ofstream f(path);
    for (const Vec3& p : x) f << "v " << p.x << " " << p.y << " " << p.z << "\n";
    for (int i = 0; i + 1 < n; i++)
        for (int j = 0; j + 1 < n; j++)
            f << "f " << index(i, j) + 1 << " " << index(i, j + 1) + 1 << " " << index(i + 1, j + 1) + 1 << " " << index(i + 1, j) + 1 << "\n";
}
