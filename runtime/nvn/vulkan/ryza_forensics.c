#include "ryza_forensics.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define RYZA_GIT_COMMIT "798e4b5-p2"
#define RYZA_BUILD_TIMESTAMP __DATE__ " " __TIME__

RyzaDrawForensic g_ryza_captured_draws[RYZA_MAX_CAPTURED_DRAWS] = {0};
uint32_t g_ryza_captured_draw_count = 0;
uint64_t g_ryza_3d_draw_count = 0;

static const char *s_body_parts[] = {
    "Environment / Billboard",
    "Environment / Small Prop",
    "Environment / Water / Terrain",
    "Environment / Grass / Foliage",
    "Environment / Structure",
    "Environment / Decal / Effect",
    "Environment / Shadow Volume",
    "Environment / Light Beam",
    "Ryza Boots / Lower Legs",
    "Ryza Thighs / Shorts",
    "Ryza Waist / Belt / Pouch",
    "Ryza Sleeves / Arms",
    "Ryza Upper Body / Jacket",
    "Ryza Hair / Head / Hat",
    "Ryza Staff / Weapon",
    "Companion / NPC Mesh",
    "Ryza Main Skinned Body",
    "Ryza Skinned Torso",
    "Ryza Skinned Hair",
    "Ryza Skinned Cape/Coat",
    "Ryza Skinned Thigh Highs",
    "Ryza Skinned Face",
    "Ryza Skinned Hands",
    "Ryza Skinned Accessories",
    "Companion Skinned Body",
    "Companion Skinned Head",
    "Enemy / Monster Skinned Mesh",
    "NPC High-Poly Skinned Mesh",
    "Bone Skinned Mesh #1",
    "Bone Skinned Mesh #2",
    "Bone Skinned Mesh #3",
    "Bone Skinned Mesh #4"
};

const char *ryza_get_build_fingerprint(void) {
    static char buf[128];
    static bool init = false;
    if (!init) {
        snprintf(buf, sizeof(buf), "%s (%s) [precomputed-render-pipeline]", RYZA_GIT_COMMIT, RYZA_BUILD_TIMESTAMP);
        init = true;
    }
    return buf;
}

void ryza_forensics_init(void) {
    g_ryza_captured_draw_count = 0;
    g_ryza_3d_draw_count = 0;
    memset(g_ryza_captured_draws, 0, sizeof(g_ryza_captured_draws));
}

RyzaDrawForensic *ryza_allocate_capture_slot(uint32_t icount, uint32_t attr_mask, bool has_bones) {
    static uint32_t s_idx_small = 0;      // 0..7: small objects (icount < 300)
    static uint32_t s_idx_med = 8;        // 8..15: medium meshes (300 <= icount < 1000)
    static uint32_t s_idx_large = 16;     // 16..27: large character meshes (icount >= 1000)
    static uint32_t s_idx_bones = 28;     // 28..31: meshes with bone attributes

    uint32_t slot = 0;
    if (has_bones) {
        slot = s_idx_bones++;
        if (s_idx_bones > 31) s_idx_bones = 28;
    } else if (icount >= 1000) {
        slot = s_idx_large++;
        if (s_idx_large > 27) s_idx_large = 16;
    } else if (icount >= 300) {
        slot = s_idx_med++;
        if (s_idx_med > 15) s_idx_med = 8;
    } else {
        slot = s_idx_small++;
        if (s_idx_small > 7) s_idx_small = 0;
    }

    RyzaDrawForensic *d = &g_ryza_captured_draws[slot];
    memset(d, 0, sizeof(*d));
    d->is_valid = true;

    snprintf(d->body_part, sizeof(d->body_part), "%s", s_body_parts[slot]);

    if (slot >= g_ryza_captured_draw_count) {
        g_ryza_captured_draw_count = slot + 1;
    }
    g_ryza_3d_draw_count++;
    return d;
}

void ryza_export_diagnostic_log(void) {
    const char *paths[] = {
        "/sdcard/Download/ryza_forensic_dump.txt",
        "/sdcard/Android/data/com.retro.botwrecompandroid/files/ryza_forensic_dump.txt",
        "/data/data/com.retro.botwrecompandroid/files/ryza_forensic_dump.txt"
    };

    FILE *f = NULL;
    for (size_t p = 0; p < sizeof(paths) / sizeof(paths[0]); p++) {
        f = fopen(paths[p], "w");
        if (f) break;
    }
    if (!f) return;

    fprintf(f, "=======================================================\n");
    fprintf(f, "RYZA NATIVE ANDROID RUNTIME SKINNING FORENSIC DUMP\n");
    fprintf(f, "Build Fingerprint: %s\n", ryza_get_build_fingerprint());
    fprintf(f, "Total 3D Draws Encountered: %llu\n", (unsigned long long)g_ryza_3d_draw_count);
    fprintf(f, "Total 3D Draws Captured: %u\n", g_ryza_captured_draw_count);
    fprintf(f, "=======================================================\n\n");

    for (uint32_t i = 0; i < RYZA_MAX_CAPTURED_DRAWS; i++) {
        RyzaDrawForensic *d = &g_ryza_captured_draws[i];
        if (!d->is_valid) continue;

        fprintf(f, "--- [DRAW #%u: %s] ---\n", i, d->body_part);
        fprintf(f, "  Frame: %llu | DrawIdx: %u | VS: 0x%llx | FS: 0x%llx (Hash: 0x%016llx)\n",
                (unsigned long long)d->frame, d->draw_idx,
                (unsigned long long)d->vs_code, (unsigned long long)d->fs_code,
                (unsigned long long)d->fs_hash);
        fprintf(f, "  AttrMask: 0x%08x | NAttrib: %u | NStream: %u\n",
                d->attr_mask, d->nattrib, d->nstream);
        fprintf(f, "  IndexBuffer: Type=%u, Count=%u, Addr=0x%llx, Range=[%u, %u], BaseVtx=%d\n",
                d->itype, d->icount, (unsigned long long)d->iaddr, d->min_idx, d->max_idx, d->base_vertex);

        fprintf(f, "  Attributes Layout:\n");
        for (uint32_t a = 0; a < RYZA_MAX_ATTRS; a++) {
            if (d->attr_mask & (1u << a)) {
                fprintf(f, "    Attr[%2u]: stream=%u, nvn_fmt=0x%02x, vk_fmt=%u, kind=%u, off=%u, stride=%u, buf=0x%llx (sz=%llu)\n",
                        a, d->attrs[a].stream, d->attrs[a].nvn_fmt, d->attrs[a].vk_fmt,
                        d->attrs[a].kind, d->attrs[a].off, d->attrs[a].stride,
                        (unsigned long long)d->attrs[a].buf_addr, (unsigned long long)d->attrs[a].buf_size);
                fprintf(f, "              raw=[0x%08x, 0x%08x, 0x%08x, 0x%08x] flt=[%.4f, %.4f, %.4f, %.4f]\n",
                        d->attrs[a].raw_data[0], d->attrs[a].raw_data[1], d->attrs[a].raw_data[2], d->attrs[a].raw_data[3],
                        d->attrs[a].float_data[0], d->attrs[a].float_data[1], d->attrs[a].float_data[2], d->attrs[a].float_data[3]);
            }
        }

        fprintf(f, "  Sampled Vertices (%u samples):\n", d->num_samples);
        for (uint32_t s = 0; s < d->num_samples; s++) {
            RyzaVertexSample *vs = &d->samples[s];
            fprintf(f, "    Sample[%u] VtxIdx=%u | Corrupted=%s | W_Valid=%s\n",
                    s, vs->vertex_idx, vs->is_corrupted ? "YES (HORIZONTAL STRETCH)" : "NO",
                    vs->w_valid ? "YES" : "NO (W<=0 or Inf/NaN)");
            fprintf(f, "      Pos:  [%.4f, %.4f, %.4f, %.4f]\n",
                    vs->pos[0], vs->pos[1], vs->pos[2], vs->pos[3]);
            fprintf(f, "      Norm: [%.4f, %.4f, %.4f, %.4f]\n",
                    vs->norm[0], vs->norm[1], vs->norm[2], vs->norm[3]);
            fprintf(f, "      BoneIndices: [%u, %u, %u, %u]\n",
                    vs->bone_indices[0], vs->bone_indices[1], vs->bone_indices[2], vs->bone_indices[3]);
            fprintf(f, "      BoneWeights: [%.4f, %.4f, %.4f, %.4f]\n",
                    vs->bone_weights[0], vs->bone_weights[1], vs->bone_weights[2], vs->bone_weights[3]);
            fprintf(f, "      Raw Attr0: [%.4f, %.4f, %.4f, %.4f]\n",
                    vs->attr_raw[0][0], vs->attr_raw[0][1], vs->attr_raw[0][2], vs->attr_raw[0][3]);
            fprintf(f, "      Raw Attr1: [%.4f, %.4f, %.4f, %.4f]\n",
                    vs->attr_raw[1][0], vs->attr_raw[1][1], vs->attr_raw[1][2], vs->attr_raw[1][3]);
            fprintf(f, "      Raw Attr2: [%.4f, %.4f, %.4f, %.4f]\n",
                    vs->attr_raw[2][0], vs->attr_raw[2][1], vs->attr_raw[2][2], vs->attr_raw[2][3]);
            fprintf(f, "      Raw Attr3: [%.4f, %.4f, %.4f, %.4f]\n",
                    vs->attr_raw[3][0], vs->attr_raw[3][1], vs->attr_raw[3][2], vs->attr_raw[3][3]);
            fprintf(f, "      ClipSpace:   [X=%.4f, Y=%.4f, Z=%.4f, W=%.4f] (X/W=%.4f)\n",
                    vs->clip_pos[0], vs->clip_pos[1], vs->clip_pos[2], vs->clip_pos[3],
                    vs->clip_pos[3] != 0.0f ? vs->clip_pos[0] / vs->clip_pos[3] : 999999.0f);
        }

        if (d->vs_glsl[0]) {
            fprintf(f, "  Vertex Shader GLSL Snippet:\n");
            // Cetak baris per baris hingga 40 baris pertama
            const char *cur = d->vs_glsl;
            int line_count = 0;
            while (*cur && line_count < 40) {
                const char *next = strchr(cur, '\n');
                if (next) {
                    fprintf(f, "    %.*s\n", (int)(next - cur), cur);
                    cur = next + 1;
                } else {
                    fprintf(f, "    %s\n", cur);
                    break;
                }
                line_count++;
            }
        }

        fprintf(f, "  Rasterization/Depth:\n");
        fprintf(f, "    CullMode=%u, FrontFace=%u, DepthTest=%u, DepthWrite=%u, DepthOp=%u\n\n",
                d->cull_mode, d->front_face, d->depth_test, d->depth_write, d->depth_compare_op);
    }

    fclose(f);
}
