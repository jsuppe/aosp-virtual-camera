/**
 * cube_geometry.h - Golden cube vertex/index data
 */
#pragma once

#include <vector>
#include <array>

#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>

struct Vertex {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec3 color;
};

class CubeGeometry {
public:
    // Golden color: RGB(255, 215, 0) normalized
    static constexpr glm::vec3 GOLD = {1.0f, 0.843f, 0.0f};
    static constexpr glm::vec3 GOLD_DARK = {0.8f, 0.674f, 0.0f};
    static constexpr glm::vec3 GOLD_LIGHT = {1.0f, 0.9f, 0.4f};
    
    static std::vector<Vertex> getVertices() {
        // Cube with proper normals for lighting
        // Each face has 4 vertices with face normal
        return {
            // Front face (Z+)
            {{-0.5f, -0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, GOLD},
            {{ 0.5f, -0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, GOLD},
            {{ 0.5f,  0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, GOLD_LIGHT},
            {{-0.5f,  0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, GOLD_LIGHT},
            
            // Back face (Z-)
            {{ 0.5f, -0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, GOLD},
            {{-0.5f, -0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, GOLD},
            {{-0.5f,  0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, GOLD_LIGHT},
            {{ 0.5f,  0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, GOLD_LIGHT},
            
            // Right face (X+)
            {{ 0.5f, -0.5f,  0.5f}, { 1.0f,  0.0f,  0.0f}, GOLD},
            {{ 0.5f, -0.5f, -0.5f}, { 1.0f,  0.0f,  0.0f}, GOLD},
            {{ 0.5f,  0.5f, -0.5f}, { 1.0f,  0.0f,  0.0f}, GOLD_LIGHT},
            {{ 0.5f,  0.5f,  0.5f}, { 1.0f,  0.0f,  0.0f}, GOLD_LIGHT},
            
            // Left face (X-)
            {{-0.5f, -0.5f, -0.5f}, {-1.0f,  0.0f,  0.0f}, GOLD_DARK},
            {{-0.5f, -0.5f,  0.5f}, {-1.0f,  0.0f,  0.0f}, GOLD_DARK},
            {{-0.5f,  0.5f,  0.5f}, {-1.0f,  0.0f,  0.0f}, GOLD},
            {{-0.5f,  0.5f, -0.5f}, {-1.0f,  0.0f,  0.0f}, GOLD},
            
            // Top face (Y+)
            {{-0.5f,  0.5f,  0.5f}, { 0.0f,  1.0f,  0.0f}, GOLD_LIGHT},
            {{ 0.5f,  0.5f,  0.5f}, { 0.0f,  1.0f,  0.0f}, GOLD_LIGHT},
            {{ 0.5f,  0.5f, -0.5f}, { 0.0f,  1.0f,  0.0f}, GOLD_LIGHT},
            {{-0.5f,  0.5f, -0.5f}, { 0.0f,  1.0f,  0.0f}, GOLD_LIGHT},
            
            // Bottom face (Y-)
            {{-0.5f, -0.5f, -0.5f}, { 0.0f, -1.0f,  0.0f}, GOLD_DARK},
            {{ 0.5f, -0.5f, -0.5f}, { 0.0f, -1.0f,  0.0f}, GOLD_DARK},
            {{ 0.5f, -0.5f,  0.5f}, { 0.0f, -1.0f,  0.0f}, GOLD_DARK},
            {{-0.5f, -0.5f,  0.5f}, { 0.0f, -1.0f,  0.0f}, GOLD_DARK},
        };
    }
    
    static std::vector<uint16_t> getIndices() {
        return {
            // Front
            0, 1, 2,  2, 3, 0,
            // Back
            4, 5, 6,  6, 7, 4,
            // Right
            8, 9, 10,  10, 11, 8,
            // Left
            12, 13, 14,  14, 15, 12,
            // Top
            16, 17, 18,  18, 19, 16,
            // Bottom
            20, 21, 22,  22, 23, 20,
        };
    }
};
