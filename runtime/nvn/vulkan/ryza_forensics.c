#include "ryza_forensics.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define RYZA_GIT_COMMIT "798e4b5-diag"
#define RYZA_BUILD_TIMESTAMP __DATE__ " " __TIME__

RyzaDrawForensic g_ryza_captured_draws[RYZA_MAX_CAPTURED_DRAWS] = {0};
uint32_t g_ryza_captured_draw_count = 0;
uint64_t g_ryza_3d_draw_count = 0;

static const char *s_body_parts[] = {
    "Ryza Boots / Lower Legs",
    "Ryza Thighs / Shorts",
    "Ryza Waist / Belt / Pouch",
    "Ryza Sleeves / Arms",
    "Ryza Upper Body / Jacket",
    "Ryza Hair / Head / Hat",
    "Ryza Staff / Weapon",
    "Companion / NPC Mesh",
    "3D Skinned Prop",
    "Static 3D Environment"
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

    for (uint32_t i = 0; i < g_ryza_captured_draw_count; i++) {
        RyzaDrawForensic *d = &g_ryza_captured_draws[i];
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
            fprintf(f, "      ClipSpace:   [X=%.4f, Y=%.4f, Z=%.4f, W=%.4f] (X/W=%.4f)\n",
                    vs->clip_pos[0], vs->clip_pos[1], vs->clip_pos[2], vs->clip_pos[3],
                    vs->clip_pos[3] != 0.0f ? vs->clip_pos[0] / vs->clip_pos[3] : 999999.0f);
        }

        fprintf(f, "  Rasterization/Depth:\n");
        fprintf(f, "    CullMode=%u, FrontFace=%u, DepthTest=%u, DepthWrite=%u, DepthOp=%u\n\n",
                d->cull_mode, d->front_face, d->depth_test, d->depth_write, d->depth_compare_op);
    }

    fclose(f);
}

void ryza_capture_draw_state(uint64_t frame, uint32_t draw_idx, uint64_t vs_code, uint64_t fs_code,
                             uint64_t fs_hash, uint32_t attr_mask, uint32_t nattrib, uint32_t nstream,
                             bool indexed, uint32_t icount, uint32_t itype, uint64_t iaddr,
                             int32_t base_vertex) {
    // Saring hanya draw call 3D karakter (berkarakteristik: indexed, icount > 100, attr_mask memiliki > 3 atribut)
    if (!indexed || icount < 100 || (attr_mask & 0xFFFFFFF8u) == 0) {
        return;
    }

    g_ryza_3d_draw_count++;

    // Simpan hingga RYZA_MAX_CAPTURED_DRAWS sampel
    uint32_t slot = g_ryza_captured_draw_count % RYZA_MAX_CAPTURED_DRAWS;
    RyzaDrawForensic *d = &g_ryza_captured_draws[slot];
    memset(d, 0, sizeof(*d));

    d->frame = frame;
    d->draw_idx = draw_idx;
    d->vs_code = vs_code;
    d->fs_code = fs_code;
    d->fs_hash = fs_hash;
    d->attr_mask = attr_mask;
    d->nattrib = nattrib;
    d->nstream = nstream;
    d->itype = itype;
    d->icount = icount;
    d->iaddr = iaddr;
    d->base_vertex = base_vertex;

    // Tentukan label bagian tubuh berdasarkan urutan kemunculan draw call karakter
    uint32_t part_idx = (g_ryza_captured_draw_count < 10) ? g_ryza_captured_draw_count : 9;
    snprintf(d->body_part, sizeof(d->body_part), "%s", s_body_parts[part_idx]);

    // Tandai valid
    d->is_valid = true;
    if (g_ryza_captured_draw_count < RYZA_MAX_CAPTURED_DRAWS) {
        g_ryza_captured_draw_count++;
    }

    // Cetak secara berkala ke stderr (yang masuk ke botwrecomp-log.txt)
    static uint64_t last_log_frame = 0;
    if (frame != last_log_frame && (g_ryza_3d_draw_count <= 20 || (g_ryza_3d_draw_count % 300 == 0))) {
        last_log_frame = frame;
        fprintf(stderr, "[RYZA-FORENSIC] Frame %llu Draw #%u (%s): VS=0x%llx FS=0x%llx AttrMask=0x%08x ICount=%u Range=[%u, %u]\n",
                (unsigned long long)frame, draw_idx, d->body_part,
                (unsigned long long)vs_code, (unsigned long long)fs_code,
                attr_mask, icount, d->min_idx, d->max_idx);
        ryza_export_diagnostic_log();
    }
}
