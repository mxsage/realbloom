#pragma once

#include <string>
#include <mutex>
#include <array>
#include <vector>
#include <memory>
#include <cstdint>

#ifndef GLEW_STATIC
#define GLEW_STATIC
#endif
#include <GL/glew.h>

#include "CMS.h"
#include "OcioShader.h"
#include "../Utils/OpenGL/GlTexture.h"
#include "../Utils/OpenGL/GlFrameBuffer.h"
#include "../Utils/OpenGL/GlUtils.h"
#include "../Utils/Misc.h"

// Color Managed Image
// Internal format is always RGBA32F
class CmImage
{
private:
    std::string m_id;
    std::string m_name;
    std::string m_sourceName = "";

    uint32_t m_width, m_height;
    bool m_useExposure = true;
    bool m_useGlobalFB = true;

    std::vector<float> m_imageData;

    std::mutex m_mutex;

    uint32_t m_oldWidth = 0, m_oldHeight = 0;
    std::shared_ptr<GlTexture> m_texture = nullptr;

    static std::shared_ptr<GlFrameBuffer> s_frameBuffer;
    std::shared_ptr<GlFrameBuffer> m_localFrameBuffer = nullptr;

    bool m_moveToGpu = true;
    void moveToGPU_Internal();

public:
    CmImage(
        const std::string& id,
        const std::string& name,
        uint32_t width = 32,
        uint32_t height = 32,
        std::array<float, 4> fillColor = { 0, 0, 0, 1 },
        bool useExposure = true,
        bool useGlobalFB = true
    );
    ~CmImage();

    const std::string& getID() const;
    const std::string& getName() const;

    const std::string& getSourceName() const;
    void setSourceName(const std::string& sourceName);

    uint32_t getWidth() const;
    uint32_t getHeight() const;

    // Number of elements in imageData
    uint32_t getImageDataSize() const;

    // RGBA. Every 4 elements represent a pixel.
    float* getImageData();

    GLuint getGlTexture();

    void lock();
    void unlock();

    void moveToGPU();

    // shouldLock must be false if the image is already locked.
    void resize(uint32_t newWidth, uint32_t newHeight, bool shouldLock);

    void fill(std::array<float, 4> color, bool shouldLock);
    void fill(std::vector<float> buffer, bool shouldLock);
    void fill(float* buffer, bool shouldLock);
    void renderUV();

    /// <summary>
    ///     Apply view transform on a given buffer
    /// </summary>
    /// <param name="buffer"></param>
    /// <param name="width"></param>
    /// <param name="height"></param>
    /// <param name="exposure"></param>
    /// <param name="texture">GlTexture instance to use for storing the output in CPU mode, or doing color transforms in GPU mode</param>
    /// <param name="frameBuffer">GlFrameBuffer instance to store the color transform output in GPU mode</param>
    /// <param name="recreate">Force recreate the texture and the frame buffer, even if they have the right size and aren't null</param>
    /// <param name="uploadToGpu">Use the texture and the frame buffer to store the result. Disabling this will force enable CPU mode and leave texture and frameBuffer untouched</param>
    /// <param name="readback">Read back the result to outBuffer</param>
    /// <param name="outBuffer">Buffer for storing the result</param>
    static void applyViewTransform(
        float* buffer,
        uint32_t width,
        uint32_t height,
        float exposure,
        std::shared_ptr<GlTexture>& texture,
        std::shared_ptr<GlFrameBuffer>& frameBuffer,
        bool recreate,
        bool uploadToGPU = true,
        bool readback = false,
        std::vector<float>* outBuffer = nullptr);

    static void cleanUp();

};
