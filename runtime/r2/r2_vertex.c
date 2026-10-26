/* The prelit vertex function of the VU1 main program (routines 0x06D0 / 0x0960 / 0x0BE0, notes/wip/r2-m1.md) in C: the
 * twin of shaders/r2.hlsl's vertex shader, used by the in-game check (bracket mode, PS2_R2_CHECK) and offline tools
 * (scripts/r2_mesh_check.c) to compare R2's vertices with what the VU XGKICKs. Floats round to nearest here (as on the
 * GPU); only the two steps where the VU's truncation decides an integer (the float-bits XYZ and fog words) truncate. */
#include "r2.h"

#include <math.h>
#include <string.h>

void r2_matmul(const float w[16], const float c[16], float out[16]) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[i * 4 + j] = w[i * 4 + 0] * c[0 * 4 + j] + w[i * 4 + 1] * c[1 * 4 + j] + w[i * 4 + 2] * c[2 * 4 + j] +
                             w[i * 4 + 3] * c[3 * 4 + j];
}

/* a + b as the VU's FMAC leaves it: rounded toward zero to float; the result's bit pattern */
static uint32_t add_rz_bits(double a, double b) {
    double s = a + b;
    float f = (float)s;
    if (fabs((double)f) > fabs(s)) f = nextafterf(f, 0.0f);
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

void r2_vertex_eval(const r2_vtx* v, const float m[16], const r2_frame_block* fb, const r2_batch* b, int csel,
                    float fade, r2_gsv* o) {
    float cx = v->x * m[0] + v->y * m[4] + v->z * m[8] + m[12];
    float cy = v->x * m[1] + v->y * m[5] + v->z * m[9] + m[13];
    float cz = v->x * m[2] + v->y * m[6] + v->z * m[10] + m[14];
    float cw = v->x * m[3] + v->y * m[7] + v->z * m[11] + m[15];
    float q = 1.0f / cw;
    float qx = cx * q, qy = cy * q, qz = cz * q;
    uint32_t bx = add_rz_bits(fb->c1[0], (double)(fb->c0[0] * qx));
    uint32_t by = add_rz_bits(fb->c1[1], (double)(fb->c0[1] * qy));
    uint32_t bz = add_rz_bits(fb->c1[2], (double)(fb->c0[2] * qz));
    o->x = (int32_t)(bx & 0xFFFF);
    o->y = (int32_t)(by & 0xFFFF);
    o->z = (bz >> 4) & 0xFFFFFF;
    float wf;
    memcpy(&wf, &v->wbits, 4);
    float r = (cw < b->fog_wmax ? cw : b->fog_wmax) * q;
    uint32_t bw = add_rz_bits(wf, (double)(b->fog_scale * r));
    o->f = (bw >> 4) & 0xFF;
    o->adc = (int)((v->wbits >> 15) & 1);
    float s = (float)v->u * (1.0f / 4096.0f), t = (float)v->v * (1.0f / 4096.0f);
    o->s = s * q;
    o->t = t * q;
    o->q = q;
    uint32_t c = v->col[csel ? 1 : 0];
    if (fade < 1.0f) {                   /* 0x0BE0: colour A alpha, colour B RGB scaled, truncated */
        uint32_t ch[4] = { c & 255, (c >> 8) & 255, (c >> 16) & 255, c >> 24 };
        if (!csel) ch[3] = (uint32_t)(int32_t)((float)ch[3] * fade);
        else for (int k = 0; k < 3; k++) ch[k] = (uint32_t)(int32_t)((float)ch[k] * fade);
        c = ch[0] | ch[1] << 8 | ch[2] << 16 | ch[3] << 24;
    }
    o->rgba = c;
}
