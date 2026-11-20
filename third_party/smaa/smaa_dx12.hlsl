// DX12 wrapper for iryoku SMAA 1x (HIGH, luma edges).
#define SMAA_CUSTOM_SL 1
#define SMAA_PRESET_HIGH 1
#define SMAA_INCLUDE_VS 1
#define SMAA_INCLUDE_PS 1

cbuffer SmaaCB : register(b0) {
    float4 gSmaaRtMetrics; // xy = 1/size, zw = size
};

#define SMAA_RT_METRICS gSmaaRtMetrics

SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);

#define SMAATexture2D(t) Texture2D t
#define SMAATexturePass2D(t) t
#define SMAASampleLevelZero(t, coord) t.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(t, coord) t.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(t, coord, offset) t.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(t, coord) t.Sample(LinearSampler, coord)
#define SMAASamplePoint(t, coord) t.Sample(PointSampler, coord)
#define SMAASampleOffset(t, coord, offset) t.Sample(LinearSampler, coord, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#define SMAAGather(t, coord) t.Gather(LinearSampler, coord, 0)

#include "SMAA.hlsl"

Texture2D colorTex : register(t0);
Texture2D edgesTex : register(t1);
Texture2D blendTex : register(t2);
Texture2D areaTex : register(t3);
Texture2D searchTex : register(t4);

float2 OversizedTriangleUV(uint vid) {
    return float2((vid << 1) & 2, vid & 2);
}

// --- Pass A: edge detection ---
struct EdgeOut {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 offset0 : TEXCOORD1;
    float4 offset1 : TEXCOORD2;
    float4 offset2 : TEXCOORD3;
};

EdgeOut EdgeVS(uint vid : SV_VertexID) {
    EdgeOut o;
    o.uv = OversizedTriangleUV(vid);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    float4 offs[3];
    SMAAEdgeDetectionVS(o.uv, offs);
    o.offset0 = offs[0];
    o.offset1 = offs[1];
    o.offset2 = offs[2];
    return o;
}

float4 EdgePS(EdgeOut pin) : SV_TARGET {
    float4 offs[3] = {pin.offset0, pin.offset1, pin.offset2};
    float2 e = SMAALumaEdgeDetectionPS(pin.uv, offs, colorTex);
    return float4(e, 0.0, 1.0);
}

// --- Pass B: blending weights ---
struct BlendOut {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
    float2 pixcoord : TEXCOORD1;
    float4 offset0 : TEXCOORD2;
    float4 offset1 : TEXCOORD3;
    float4 offset2 : TEXCOORD4;
};

BlendOut BlendVS(uint vid : SV_VertexID) {
    BlendOut o;
    o.uv = OversizedTriangleUV(vid);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    float2 pix;
    float4 offs[3];
    SMAABlendingWeightCalculationVS(o.uv, pix, offs);
    o.pixcoord = pix;
    o.offset0 = offs[0];
    o.offset1 = offs[1];
    o.offset2 = offs[2];
    return o;
}

float4 BlendPS(BlendOut pin) : SV_TARGET {
    float4 offs[3] = {pin.offset0, pin.offset1, pin.offset2};
    return SMAABlendingWeightCalculationPS(pin.uv, pin.pixcoord, offs, edgesTex, areaTex, searchTex,
                                           float4(0, 0, 0, 0));
}

// --- Pass C: neighborhood blend ---
struct NeighborhoodOut {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 offset : TEXCOORD1;
};

NeighborhoodOut NeighborhoodVS(uint vid : SV_VertexID) {
    NeighborhoodOut o;
    o.uv = OversizedTriangleUV(vid);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    SMAANeighborhoodBlendingVS(o.uv, o.offset);
    return o;
}

float4 NeighborhoodPS(NeighborhoodOut pin) : SV_TARGET {
    return SMAANeighborhoodBlendingPS(pin.uv, pin.offset, colorTex, blendTex);
}
