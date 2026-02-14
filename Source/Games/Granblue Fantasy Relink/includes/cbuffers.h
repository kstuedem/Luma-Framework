#pragma once

// Granblue Fantasy Relink cbuffer structures
// Derived from 0xDA85F5BB compute shader (SceneBuffer at cb0)

// SceneBuffer (cb0) - 544 bytes (34 float4 registers)
// This buffer is bound during the 0xDA85F5BB shader pass which contains
// the projection matrix. We sniff it during unmap to extract jitter,
// FOV, and near/far plane values.
struct cbSceneBuffer
{
   Math::Matrix44 g_View;                   // Offset:   0  (c0-c3)
   Math::Matrix44 g_Proj;                   // Offset:  64  (c4-c7)
   Math::Matrix44 g_ViewProjection;         // Offset: 128  (c8-c11)
   Math::Matrix44 g_ViewInverseMatrix;      // Offset: 192  (c12-c15)
   Math::Matrix44 g_PrevView;               // Offset: 256  (c16-c19)
   Math::Matrix44 g_PrevProj;               // Offset: 320  (c20-c23)
   Math::Matrix44 g_PrevViewProjection;     // Offset: 384  (c24-c27)
   Math::Matrix44 g_PrevViewInverseMatrix;  // Offset: 448  (c28-c31)
   float4 g_ProjectionOffset;               // Offset: 512  (c32) - contains jitter offsets
   int g_FrameCount[4];                     // Offset: 528  (c33)
};
static_assert(sizeof(cbSceneBuffer) == 544, "cbSceneBuffer size mismatch");

// HPixel_Buffer (cb12) used by TAA shader 0x478E345C
struct cbHPixelBuffer
{
   float4 g_TargetUvParam; // Offset: 0 (c0) - xy = render target size, zw = 1/size
};

// CamParam_HPixel_Buffer (cb13) from 0xDA85F5BB
struct cbCamParam
{
   float4 g_CameraParam;   // Offset: 0  - x = near, y = far-near range
   float4 g_CameraVec;     // Offset: 16
   float4 g_CameraParam2;  // Offset: 32
};
