#include "Convolution.h"
#include "ConvolutionThread.h"
#include "ConvolutionFFT.h"

namespace RealBloom
{

    constexpr uint32_t CONV_PROG_TIMESTEP = 1000;

    void drawRect(CmImage* image, int rx, int ry, int rw, int rh);
    void fillRect(CmImage* image, int rx, int ry, int rw, int rh);

    uint32_t ConvolutionStatus::getNumChunksDone() const
    {
        return m_numChunksDone;
    }

    void ConvolutionStatus::setNumChunksDone(uint32_t numChunksDone)
    {
        m_numChunksDone = numChunksDone;
    }

    const std::string& ConvolutionStatus::getFftStage() const
    {
        return m_fftStage;
    }

    void ConvolutionStatus::setFftStage(const std::string& fftStage)
    {
        m_fftStage = fftStage;
    }

    void ConvolutionStatus::reset()
    {
        super::reset();
        m_numChunksDone = 0;
        m_fftStage = "";
    }

    Convolution::Convolution() : m_imgKernelSrc("", ""), m_imgOutput("", "")
    {}

    ConvolutionParams* Convolution::getParams()
    {
        return &m_params;
    }

    void Convolution::setImgInput(CmImage* image)
    {
        m_imgInput = image;
    }

    void Convolution::setImgKernel(CmImage* image)
    {
        m_imgKernel = image;
    }

    void Convolution::setImgConvPreview(CmImage* image)
    {
        m_imgConvPreview = image;
    }

    void Convolution::setImgConvMix(CmImage* image)
    {
        m_imgConvMix = image;
    }

    CmImage* Convolution::getImgKernelSrc()
    {
        return &m_imgKernelSrc;
    }

    void Convolution::previewThreshold(size_t* outNumPixels)
    {
        float threshold = m_params.threshold;
        float transKnee = transformKnee(m_params.knee);
        size_t numPixels = 0;

        {
            // Input image
            std::scoped_lock lock1(*m_imgInput);
            float* inputBuffer = m_imgInput->getImageData();
            uint32_t inputWidth = m_imgInput->getWidth();
            uint32_t inputHeight = m_imgInput->getHeight();

            // Convolution Preview Image
            std::scoped_lock lock2(*m_imgConvPreview);
            m_imgConvPreview->resize(inputWidth, inputHeight, false);
            float* prevBuffer = m_imgConvPreview->getImageData();

            for (uint32_t y = 0; y < inputHeight; y++)
            {
                for (uint32_t x = 0; x < inputWidth; x++)
                {
                    uint32_t redIndex = (y * inputWidth + x) * 4;
                    prevBuffer[redIndex + 0] = 0;
                    prevBuffer[redIndex + 1] = 0;
                    prevBuffer[redIndex + 2] = 0;
                    prevBuffer[redIndex + 3] = 1;

                    float v = rgbToGrayscale(inputBuffer[redIndex + 0], inputBuffer[redIndex + 1], inputBuffer[redIndex + 2]);
                    if (v > threshold)
                    {
                        numPixels++;
                        float mul = softThreshold(v, threshold, transKnee);
                        blendAddRGB(prevBuffer, redIndex, inputBuffer, redIndex, mul);
                    }
                }
            }
        }
        m_imgConvPreview->moveToGPU();

        if (outNumPixels)
            *outNumPixels = numPixels;
    }

    void Convolution::kernel(bool previewMode, std::vector<float>* outBuffer, uint32_t* outWidth, uint32_t* outHeight)
    {
        // Parameters

        std::vector<float> kernelBuffer;
        uint32_t kernelBufferSize = 0;
        uint32_t kernelWidth = 0, kernelHeight = 0;
        {
            std::scoped_lock lock(m_imgKernelSrc);
            float* kernelSrcBuffer = m_imgKernelSrc.getImageData();
            kernelBufferSize = m_imgKernelSrc.getImageDataSize();
            kernelWidth = m_imgKernelSrc.getWidth();
            kernelHeight = m_imgKernelSrc.getHeight();

            kernelBuffer.resize(kernelBufferSize);
            std::copy(kernelSrcBuffer, kernelSrcBuffer + kernelBufferSize, kernelBuffer.data());
        }

        float centerX, centerY;
        centerX = (float)kernelWidth / 2.0f;
        centerY = (float)kernelHeight / 2.0f;

        float scaleW = fmaxf(m_params.kernelScaleX, 0.01f);
        float scaleH = fmaxf(m_params.kernelScaleY, 0.01f);
        float cropW = fminf(fmaxf(m_params.kernelCropX, 0.1f), 1.0f);
        float cropH = fminf(fmaxf(m_params.kernelCropY, 0.1f), 1.0f);
        float rotation = m_params.kernelRotation;

        bool autoExposure = m_params.autoExposure;
        float expMul = applyExposure(m_params.kernelExposure);
        float contrast = m_params.kernelContrast;

        std::array<float, 3> color = m_params.kernelColor;

        uint32_t scaledWidth, scaledHeight;
        scaledWidth = (uint32_t)floorf(scaleW * (float)kernelWidth);
        scaledHeight = (uint32_t)floorf(scaleH * (float)kernelHeight);

        uint32_t croppedWidth, croppedHeight;
        croppedWidth = (uint32_t)floorf(cropW * (float)scaledWidth);
        croppedHeight = (uint32_t)floorf(cropH * (float)scaledHeight);

        float scaledCenterX, scaledCenterY;
        scaledCenterX = (scaleW * (float)kernelWidth) / 2.0f;
        scaledCenterY = (scaleH * (float)kernelHeight) / 2.0f;

        // return the result dimensions if requested
        if (!previewMode && (outBuffer == nullptr))
        {
            *outWidth = croppedWidth;
            *outHeight = croppedHeight;
            return;
        }

        // Scale and Rotation
        std::vector<float> scaledBuffer;
        uint32_t scaledBufferSize = 0;
        if ((scaleW == 1) && (scaleH == 1) && (rotation == 0))
        {
            scaledBufferSize = kernelBufferSize;
            scaledBuffer.resize(scaledBufferSize);
            std::copy(kernelBuffer.data(), kernelBuffer.data() + kernelBufferSize, scaledBuffer.data());
        }
        else
        {
            scaledBufferSize = scaledWidth * scaledHeight * 4;
            scaledBuffer.resize(scaledBufferSize);

            uint32_t redIndexKernel, redIndexScaled;
            float transX, transY;
            float transX_rot = 0, transY_rot = 0;
            float targetColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
            Bilinear bil;
            for (uint32_t y = 0; y < scaledHeight; y++)
            {
                for (uint32_t x = 0; x < scaledWidth; x++)
                {
                    transX = (x + 0.5f) / scaleW;
                    transY = (y + 0.5f) / scaleH;
                    rotatePoint(transX, transY, centerX, centerY, -rotation, transX_rot, transY_rot);
                    bil.calc(transX_rot, transY_rot);

                    // CURRENT COLOR = (KERNEL[TOPLEFT] * TOPLEFTMIX) + (KERNEL[TOPRIGHT] * TOPRIGHTMIX) + ...

                    targetColor[0] = 0;
                    targetColor[1] = 0;
                    targetColor[2] = 0;

                    if (checkBounds(bil.topLeftPos[0], bil.topLeftPos[1], kernelWidth, kernelHeight))
                    {
                        redIndexKernel = (bil.topLeftPos[1] * kernelWidth + bil.topLeftPos[0]) * 4;
                        blendAddRGB(targetColor, 0, kernelBuffer.data(), redIndexKernel, bil.topLeftWeight);
                    }
                    if (checkBounds(bil.topRightPos[0], bil.topRightPos[1], kernelWidth, kernelHeight))
                    {
                        redIndexKernel = (bil.topRightPos[1] * kernelWidth + bil.topRightPos[0]) * 4;
                        blendAddRGB(targetColor, 0, kernelBuffer.data(), redIndexKernel, bil.topRightWeight);
                    }
                    if (checkBounds(bil.bottomLeftPos[0], bil.bottomLeftPos[1], kernelWidth, kernelHeight))
                    {
                        redIndexKernel = (bil.bottomLeftPos[1] * kernelWidth + bil.bottomLeftPos[0]) * 4;
                        blendAddRGB(targetColor, 0, kernelBuffer.data(), redIndexKernel, bil.bottomLeftWeight);
                    }
                    if (checkBounds(bil.bottomRightPos[0], bil.bottomRightPos[1], kernelWidth, kernelHeight))
                    {
                        redIndexKernel = (bil.bottomRightPos[1] * kernelWidth + bil.bottomRightPos[0]) * 4;
                        blendAddRGB(targetColor, 0, kernelBuffer.data(), redIndexKernel, bil.bottomRightWeight);
                    }

                    redIndexScaled = (y * scaledWidth + x) * 4;
                    std::copy(targetColor, targetColor + 4, &(scaledBuffer[redIndexScaled]));
                }
            }
        }

        // No longer need the original buffer
        clearVector(kernelBuffer);

        // Crop
        std::vector<float> croppedBuffer;
        uint32_t croppedBufferSize = 0;
        if ((cropW == 1) && (cropH == 1))
        {
            croppedBufferSize = scaledBufferSize;
            croppedBuffer.resize(croppedBufferSize);
            std::copy(scaledBuffer.data(), scaledBuffer.data() + scaledBufferSize, croppedBuffer.data());
        }
        else
        {
            croppedBufferSize = croppedWidth * croppedHeight * 4;
            croppedBuffer.resize(croppedBufferSize);

            float cropMaxOffsetX = scaledWidth - croppedWidth;
            float cropMaxOffsetY = scaledHeight - croppedHeight;

            uint32_t cropStartX = (uint32_t)floorf(m_params.kernelCenterX * cropMaxOffsetX);
            uint32_t cropStartY = (uint32_t)floorf(m_params.kernelCenterY * cropMaxOffsetY);

            for (uint32_t y = 0; y < croppedHeight; y++)
            {
                for (uint32_t x = 0; x < croppedWidth; x++)
                {
                    uint32_t redIndexCropped = (y * croppedWidth + x) * 4;
                    uint32_t redIndexScaled = 4 * (((y + cropStartY) * scaledWidth) + x + cropStartX);

                    croppedBuffer[redIndexCropped + 0] = scaledBuffer[redIndexScaled + 0];
                    croppedBuffer[redIndexCropped + 1] = scaledBuffer[redIndexScaled + 1];
                    croppedBuffer[redIndexCropped + 2] = scaledBuffer[redIndexScaled + 2];
                    croppedBuffer[redIndexCropped + 3] = scaledBuffer[redIndexScaled + 3];
                }
            }
        }

        clearVector(scaledBuffer);

        // Apply contrast and exposure
        {
            // Get the brightest value
            float maxV = EPSILON;
            for (uint32_t i = 0; i < croppedBufferSize; i++)
            {
                float v = fmaxf(0.0f, croppedBuffer[i]);
                if ((v > maxV) && (i % 4 != 3)) // if not alpha channel
                    maxV = v;
            }

            for (uint32_t y = 0; y < croppedHeight; y++)
            {
                for (uint32_t x = 0; x < croppedWidth; x++)
                {
                    uint32_t redIndex = (y * croppedWidth + x) * 4;

                    // Normalize
                    croppedBuffer[redIndex + 0] /= maxV;
                    croppedBuffer[redIndex + 1] /= maxV;
                    croppedBuffer[redIndex + 2] /= maxV;

                    // Calculate the grayscale value
                    float grayscale = rgbToGrayscale(croppedBuffer[redIndex + 0], croppedBuffer[redIndex + 1], croppedBuffer[redIndex + 2]);

                    // Apply contrast, de-normalize, colorize
                    if (grayscale > 0.0f)
                    {
                        float mul = expMul * maxV * (contrastCurve(grayscale, contrast) / grayscale);
                        croppedBuffer[redIndex + 0] *= mul * color[0];
                        croppedBuffer[redIndex + 1] *= mul * color[1];
                        croppedBuffer[redIndex + 2] *= mul * color[2];
                    }
                }
            }
        }

        // Copy to kernel preview image
        {
            std::scoped_lock lock(*m_imgKernel);
            m_imgKernel->resize(croppedWidth, croppedHeight, false);
            float* prevBuffer = m_imgKernel->getImageData();
            std::copy(croppedBuffer.data(), croppedBuffer.data() + croppedBufferSize, prevBuffer);
        }

        bool outerRequest = (!previewMode && (outBuffer != nullptr));

        // Auto-adjust the exposure
        if (autoExposure && outerRequest)
        {
            // Get the sum of the grayscale values
            float sumV = 0.0f;
            for (uint32_t y = 0; y < croppedHeight; y++)
            {
                for (uint32_t x = 0; x < croppedWidth; x++)
                {
                    uint32_t redIndex = (y * croppedWidth + x) * 4;

                    float grayscale = rgbToGrayscale(croppedBuffer[redIndex + 0], croppedBuffer[redIndex + 1], croppedBuffer[redIndex + 2]);
                    sumV += fmaxf(0.0f, grayscale);
                }
            }

            // To avoid division by zero
            sumV = fmaxf(EPSILON, sumV);

            // Divide by the sum, and cancel out the convolution multiplier
            float mul = 1.0 / ((double)sumV * (double)CONV_MULTIPLIER);
            for (uint32_t i = 0; i < croppedBufferSize; i++)
            {
                if (i % 4 != 3) // if not alpha channel
                    croppedBuffer[i] *= mul;
            }
        }

        // Copy to outBuffer if it's requested by another function
        if (outerRequest)
        {
            *outWidth = croppedWidth;
            *outHeight = croppedHeight;
            outBuffer->resize(croppedBufferSize);
            *outBuffer = croppedBuffer;
        }

        // Preview center point
        if (m_params.kernelPreviewCenter)
        {
            int newCenterX = (int)floorf(m_params.kernelCenterX * (float)croppedWidth);
            int newCenterY = (int)floorf(m_params.kernelCenterY * (float)croppedHeight);

            int prevSquareSize = (int)roundf(0.15f * fminf((float)croppedWidth, (float)croppedHeight));
            if (prevSquareSize % 2 == 0) prevSquareSize += 1;

            drawRect(
                m_imgKernel,
                newCenterX - (prevSquareSize / 2),
                newCenterY - (prevSquareSize / 2),
                prevSquareSize, prevSquareSize);

            fillRect(m_imgKernel, newCenterX - 2, newCenterY - 2, 5, 5);
        }

        m_imgKernel->moveToGPU();
        m_imgKernel->setSourceName(m_imgKernelSrc.getSourceName());
    }

    void Convolution::mix(bool additive, float inputMix, float convMix, float mix, float convExposure)
    {
        inputMix = fmaxf(inputMix, 0.0f);
        convMix = fmaxf(convMix, 0.0f);
        if (!additive)
        {
            mix = fminf(fmaxf(mix, 0.0f), 1.0f);
            convMix = mix;
            inputMix = 1.0f - mix;
        }

        {
            // Input buffer
            std::scoped_lock lock1(*m_imgInput);
            float* inputBuffer = m_imgInput->getImageData();
            uint32_t inputWidth = m_imgInput->getWidth();
            uint32_t inputHeight = m_imgInput->getHeight();

            // Conv Buffer
            std::scoped_lock lock2(m_imgOutput);
            float* convBuffer = m_imgOutput.getImageData();
            uint32_t convWidth = m_imgOutput.getWidth();
            uint32_t convHeight = m_imgOutput.getHeight();

            if ((inputWidth == convWidth) && (inputHeight == convHeight))
            {
                // Conv Mix Buffer
                std::scoped_lock convMixImageLock(*m_imgConvMix);
                m_imgConvMix->resize(inputWidth, inputHeight, false);
                float* convMixBuffer = m_imgConvMix->getImageData();

                float multiplier = convMix * applyExposure(convExposure);
                for (uint32_t y = 0; y < inputHeight; y++)
                {
                    for (uint32_t x = 0; x < inputWidth; x++)
                    {
                        uint32_t redIndex = (y * inputWidth + x) * 4;

                        convMixBuffer[redIndex + 0] =
                            (inputBuffer[redIndex + 0] * inputMix) + (convBuffer[redIndex + 0] * multiplier);
                        convMixBuffer[redIndex + 1] =
                            (inputBuffer[redIndex + 1] * inputMix) + (convBuffer[redIndex + 1] * multiplier);
                        convMixBuffer[redIndex + 2] =
                            (inputBuffer[redIndex + 2] * inputMix) + (convBuffer[redIndex + 2] * multiplier);

                        convMixBuffer[redIndex + 3] = 1;
                    }
                }
            }
        }
        m_imgConvMix->moveToGPU();
    }

    void Convolution::convolve()
    {
        // If there's already a convolution process going on
        cancel();

        // Capture the parameters to avoid changes during the process
        m_capturedParams = m_params;

        // Reset the status
        m_status.reset();
        m_status.setWorking();
        m_status.setFftStage("Initializing");

        // Start the main thread
        m_thread = std::make_shared<std::jthread>([this]()
            {
                // Kernel Buffer
                std::vector<float> kernelBuffer;
                uint32_t kernelWidth = 0, kernelHeight = 0;
                kernel(false, &kernelBuffer, &kernelWidth, &kernelHeight);

                // Input Buffer
                std::vector<float> inputBuffer;
                uint32_t inputWidth = 0, inputHeight = 0;
                uint32_t inputBufferSize = 0;
                {
                    // Input buffer from input image
                    std::scoped_lock lock(*m_imgInput);
                    inputWidth = m_imgInput->getWidth();
                    inputHeight = m_imgInput->getHeight();
                    inputBufferSize = m_imgInput->getImageDataSize();
                    float* inputImageData = m_imgInput->getImageData();

                    // Copy the buffer so the input image doesn't stay locked throughout the process
                    inputBuffer.resize(inputBufferSize);
                    std::copy(inputImageData, inputImageData + inputBufferSize, inputBuffer.data());
                }

                // Call the appropriate function

                if (m_capturedParams.methodInfo.method == ConvolutionMethod::FFT_CPU)
                    convFftCPU(
                        kernelBuffer, kernelWidth, kernelHeight,
                        inputBuffer, inputWidth, inputHeight, inputBufferSize);

                if (m_capturedParams.methodInfo.method == ConvolutionMethod::FFT_GPU)
                    convFftGPU(
                        kernelBuffer, kernelWidth, kernelHeight,
                        inputBuffer, inputWidth, inputHeight, inputBufferSize);

                if (m_capturedParams.methodInfo.method == ConvolutionMethod::NAIVE_CPU)
                    convNaiveCPU(
                        kernelBuffer, kernelWidth, kernelHeight,
                        inputBuffer, inputWidth, inputHeight, inputBufferSize);

                if (m_capturedParams.methodInfo.method == ConvolutionMethod::NAIVE_GPU)
                    convNaiveGPU(
                        kernelBuffer, kernelWidth, kernelHeight,
                        inputBuffer, inputWidth, inputHeight, inputBufferSize);

                // Update convMixParamsChanged
                if (m_status.isOK() && !m_status.mustCancel())
                {
                    Async::schedule([this]()
                        {
                            bool* pConvMixParamsChanged = (bool*)Async::getShared("convMixParamsChanged");
                            if (pConvMixParamsChanged != nullptr) *pConvMixParamsChanged = true;
                        });
                }

                m_status.setDone();
            }
        );
    }

    void Convolution::cancel()
    {
        if (!m_status.isWorking())
        {
            m_status.reset();
            return;
        }

        m_status.setMustCancel();

        if (m_capturedParams.methodInfo.method == ConvolutionMethod::NAIVE_CPU)
        {
            // Tell the sub-threads to stop
            for (auto& ct : m_threads)
                ct->stop();
        }

        // Wait for the main thread
        threadJoin(m_thread.get());
        m_thread = nullptr;

        m_status.reset();
    }

    const ConvolutionStatus& Convolution::getStatus() const
    {
        return m_status;
    }

    void Convolution::getStatusText(std::string& outStatus, std::string& outMessage, uint32_t& outMessageType) const
    {
        outStatus = "";
        outMessage = "";
        outMessageType = 0;

        if (m_status.mustCancel())
            return;

        if (!m_status.isOK())
        {
            outMessage = m_status.getError();
            outMessageType = 3;
        }
        else if (m_status.isWorking())
        {
            float elapsedSec = m_status.getElapsedSec();
            if (m_capturedParams.methodInfo.method == ConvolutionMethod::NAIVE_CPU)
            {
                uint32_t numThreads = m_threads.size();
                uint32_t numPixels = 0;
                uint32_t numDone = 0;

                for (uint32_t i = 0; i < numThreads; i++)
                {
                    ConvolutionThreadStats* stats = m_threads[i]->getStats();
                    if (stats->state == ConvolutionThreadState::Working || stats->state == ConvolutionThreadState::Done)
                    {
                        numPixels += stats->numPixels;
                        numDone += stats->numDone;
                    }
                }

                float progress = (numPixels > 0) ? (float)numDone / (float)numPixels : 1.0f;
                float remainingSec = (elapsedSec * (float)(numPixels - numDone)) / fmaxf((float)(numDone), EPSILON);
                outStatus = strFormat(
                    "%.1f%%%% (%u/%u)\n%s / %s",
                    progress * 100.0f,
                    numDone,
                    numPixels,
                    strFromElapsed(elapsedSec).c_str(),
                    strFromElapsed(remainingSec).c_str());
            }
            else if (m_capturedParams.methodInfo.method == ConvolutionMethod::NAIVE_GPU)
            {
                outStatus = strFormat(
                    "%u/%u chunks\n%s",
                    m_status.getNumChunksDone(),
                    m_capturedParams.methodInfo.numChunks,
                    strFromElapsed(elapsedSec).c_str());
            }
            else if (m_capturedParams.methodInfo.method == ConvolutionMethod::FFT_CPU)
            {
                outStatus = strFormat(
                    "%s\n%s",
                    m_status.getFftStage().c_str(),
                    strFromElapsed(elapsedSec).c_str());
            }
            else if (m_capturedParams.methodInfo.method == ConvolutionMethod::FFT_GPU)
            {
                outStatus = strFromElapsed(elapsedSec).c_str();
            }
        }
        else if (m_status.hasTimestamps())
        {
            float elapsedSec = m_status.getElapsedSec();
            outStatus = strFormat("Done (%s)", strFromDuration(elapsedSec).c_str());
        }
    }

    std::string Convolution::getResourceInfo()
    {
        // Kernel Size
        uint32_t kernelWidth = 0, kernelHeight = 0;
        kernel(false, nullptr, &kernelWidth, &kernelHeight);

        // Input Size
        uint32_t inputWidth = m_imgInput->getWidth();
        uint32_t inputHeight = m_imgInput->getHeight();

        // Estimate resource usage
        uint64_t numPixels = 0;
        uint64_t numPixelsPerBlock = 0;
        uint64_t ramUsage = 0;
        uint64_t vramUsage = 0;

        // Number of pixels that pass the threshold
        if ((m_params.methodInfo.method == ConvolutionMethod::NAIVE_CPU) || (m_params.methodInfo.method == ConvolutionMethod::NAIVE_GPU))
            previewThreshold(&numPixels);

        // Number of pixels per thread/chunk
        if (m_params.methodInfo.method == ConvolutionMethod::NAIVE_CPU)
            numPixelsPerBlock = numPixels / m_params.methodInfo.numThreads;
        else if (m_params.methodInfo.method == ConvolutionMethod::NAIVE_GPU)
            numPixelsPerBlock = numPixels / m_params.methodInfo.numChunks;

        // Input and kernel size in bytes
        uint64_t inputSizeBytes = (uint64_t)inputWidth * (uint64_t)inputHeight * 4 * sizeof(float);
        uint64_t kernelSizeBytes = (uint64_t)kernelWidth * (uint64_t)kernelHeight * 4 * sizeof(float);

        // Calculate memory usage for different methods
        if (m_params.methodInfo.method == ConvolutionMethod::FFT_CPU)
        {
            uint32_t paddedWidth, paddedHeight;
            calcFftConvPadding(
                false, false,
                inputWidth, inputHeight,
                kernelWidth, kernelHeight,
                m_params.kernelCenterX, m_params.kernelCenterY,
                paddedWidth, paddedHeight);

            ramUsage = inputSizeBytes + kernelSizeBytes + ((uint64_t)paddedWidth * (uint64_t)paddedHeight * 10 * sizeof(float));
        }
        else if (m_params.methodInfo.method == ConvolutionMethod::NAIVE_CPU)
        {
            ramUsage = inputSizeBytes + kernelSizeBytes + (inputSizeBytes * m_params.methodInfo.numThreads);
        }
        else if (m_params.methodInfo.method == ConvolutionMethod::NAIVE_GPU)
        {
            // input buffer + kernel buffer + final output buffer + output buffer for chunk
            // + (numPoints * 5) + fbData (same size as input buffer)
            ramUsage = (inputSizeBytes * 4) + kernelSizeBytes + (numPixelsPerBlock * 5 * sizeof(float));

            // (numPoints * 5) + kernel buffer + frame buffer (same size as input buffer)
            vramUsage = (numPixelsPerBlock * 5 * sizeof(float)) + kernelSizeBytes + inputSizeBytes;
        }

        // Format output
        if (m_params.methodInfo.method == ConvolutionMethod::FFT_CPU)
        {
            return strFormat(
                "Est. Memory: %s",
                strFromDataSize(ramUsage).c_str());
        }
        else if (m_params.methodInfo.method == ConvolutionMethod::FFT_GPU)
        {
            return "Undetermined";
        }
        else if (m_params.methodInfo.method == ConvolutionMethod::NAIVE_CPU)
        {
            return strFormat(
                "Total Pixels: %s\nPixels/Thread: %s\nEst. Memory: %s",
                strFromBigInteger(numPixels).c_str(),
                strFromBigInteger(numPixelsPerBlock).c_str(),
                strFromDataSize(ramUsage).c_str());
        }
        else if (m_params.methodInfo.method == ConvolutionMethod::NAIVE_GPU)
        {
            return strFormat(
                "Total Pixels: %s\nPixels/Chunk: %s\nEst. Memory: %s\nEst. VRAM: %s",
                strFromBigInteger(numPixels).c_str(),
                strFromBigInteger(numPixelsPerBlock).c_str(),
                strFromDataSize(ramUsage).c_str(),
                strFromDataSize(vramUsage).c_str());
        }

        return "";
    }

    void Convolution::convFftCPU(
        std::vector<float>& kernelBuffer,
        uint32_t kernelWidth,
        uint32_t kernelHeight,
        std::vector<float>& inputBuffer,
        uint32_t inputWidth,
        uint32_t inputHeight,
        uint32_t inputBufferSize)
    {
        try
        {
            // FFT
            ConvolutionFFT fftConv(
                m_capturedParams, inputBuffer.data(), inputWidth, inputHeight,
                kernelBuffer.data(), kernelWidth, kernelHeight
            );

            uint32_t numStages = 14;
            uint32_t currStage = 0;

            // Prepare padded buffers
            currStage++;
            m_status.setFftStage(strFormat("%u/%u Preparing", currStage, numStages));
            fftConv.pad();

            if (m_status.mustCancel()) throw std::exception();

            // Repeat for 3 color channels
            for (uint32_t i = 0; i < 3; i++)
            {
                // Input FFT
                currStage++;
                m_status.setFftStage(strFormat("%u/%u %s: Input FFT", currStage, numStages, strFromColorChannelID(i).c_str()));
                fftConv.inputFFT(i);

                if (m_status.mustCancel()) throw std::exception();

                // Kernel FFT
                currStage++;
                m_status.setFftStage(strFormat("%u/%u %s: Kernel FFT", currStage, numStages, strFromColorChannelID(i).c_str()));
                fftConv.kernelFFT(i);

                if (m_status.mustCancel()) throw std::exception();

                // Multiply the Fourier transforms
                currStage++;
                m_status.setFftStage(strFormat("%u/%u %s: Multiplying", currStage, numStages, strFromColorChannelID(i).c_str()));
                fftConv.multiply(i);

                if (m_status.mustCancel()) throw std::exception();

                // Inverse FFT
                currStage++;
                m_status.setFftStage(strFormat("%u/%u %s: Inverse FFT", currStage, numStages, strFromColorChannelID(i).c_str()));
                fftConv.inverse(i);

                if (m_status.mustCancel()) throw std::exception();
            }

            // Get the final output
            currStage++;
            m_status.setFftStage(strFormat("%u/%u Finalizing", currStage, numStages));
            fftConv.output();

            // Update the output image
            {
                std::scoped_lock lock(m_imgOutput);
                m_imgOutput.resize(inputWidth, inputHeight, false);
                float* convBuffer = m_imgOutput.getImageData();
                std::copy(
                    fftConv.getBuffer().data(),
                    fftConv.getBuffer().data() + fftConv.getBuffer().size(),
                    convBuffer
                );
            }
        }
        catch (const std::exception& e)
        {
            m_status.setError(e.what());
        }
    }

    void Convolution::convFftGPU(
        std::vector<float>& kernelBuffer,
        uint32_t kernelWidth,
        uint32_t kernelHeight,
        std::vector<float>& inputBuffer,
        uint32_t inputWidth,
        uint32_t inputHeight,
        uint32_t inputBufferSize)
    {
        // Create GpuHelper instance
        GpuHelper gpuHelper;
        std::string inpFilename = gpuHelper.getInpFilename();
        std::string outFilename = gpuHelper.getOutFilename();

        try
        {
            // Prepare input data
            BinaryConvFftGpuInput binInput;
            binInput.cp_kernelCenterX = m_capturedParams.kernelCenterX;
            binInput.cp_kernelCenterY = m_capturedParams.kernelCenterY;
            binInput.cp_convThreshold = m_capturedParams.threshold;
            binInput.cp_convKnee = m_capturedParams.knee;
            binInput.convMultiplier = CONV_MULTIPLIER;
            binInput.inputWidth = inputWidth;
            binInput.inputHeight = inputHeight;
            binInput.kernelWidth = kernelWidth;
            binInput.kernelHeight = kernelHeight;
            binInput.inputBuffer = inputBuffer;
            binInput.kernelBuffer = kernelBuffer;

            // Create the input file
            std::ofstream inpFile;
            inpFile.open(inpFilename, std::ofstream::out | std::ofstream::binary | std::ofstream::trunc);
            if (!inpFile.is_open())
                throw std::exception(
                    strFormat("Input file \"%s\" could not be created/opened.", inpFilename.c_str()).c_str()
                );

            // Write operation type
            uint32_t opType = (uint32_t)GpuHelperOperationType::ConvFFT;
            stmWriteScalar(inpFile, opType);

            // Write input data
            binInput.writeTo(inpFile);

            // Close the input file
            inpFile.flush();
            inpFile.close();

            // Execute GPU Helper
            gpuHelper.run();

            // Wait for the output file to be created
            gpuHelper.waitForOutput([this]() { return m_status.mustCancel(); });

            // Wait for the GPU Helper to finish its job
            while (gpuHelper.isRunning())
            {
                if (m_status.mustCancel())
                    throw std::exception();
                std::this_thread::sleep_for(std::chrono::milliseconds(WAIT_TIMESTEP_SHORT));
            }

            // Open the output file
            std::ifstream outFile;
            outFile.open(outFilename, std::ifstream::in | std::ifstream::binary);
            if (!outFile.is_open())
                throw std::exception(
                    strFormat("Output file \"%s\" could not be opened.", outFilename.c_str()).c_str()
                );
            else
                outFile.seekg(std::ifstream::beg);

            // Parse the output file
            BinaryConvFftGpuOutput binOutput;
            binOutput.readFrom(outFile);
            if (binOutput.status == 1)
            {
                // Update the output image

                std::scoped_lock lock(m_imgOutput);
                m_imgOutput.resize(inputWidth, inputHeight, false);
                float* convBuffer = m_imgOutput.getImageData();
                uint32_t convBufferSize = m_imgOutput.getImageDataSize();

                if (convBufferSize == binOutput.buffer.size())
                {
                    std::copy(binOutput.buffer.data(), binOutput.buffer.data() + binOutput.buffer.size(), convBuffer);
                }
                else
                {
                    m_status.setError(strFormat(
                        "Output buffer size (%u) does not match the input buffer size (%u).",
                        binOutput.buffer.size(), convBufferSize
                    ));
                }
            }
            else
            {
                m_status.setError(binOutput.error);
            }

            // Close the output file
            outFile.close();
        }
        catch (const std::exception& e)
        {
            m_status.setError(e.what());
        }

        // Clean up
        gpuHelper.cleanUp();
    }

    void Convolution::convNaiveCPU(
        std::vector<float>& kernelBuffer,
        uint32_t kernelWidth,
        uint32_t kernelHeight,
        std::vector<float>& inputBuffer,
        uint32_t inputWidth,
        uint32_t inputHeight,
        uint32_t inputBufferSize)
    {
        // Clamp the number of threads
        uint32_t maxThreads = std::thread::hardware_concurrency();
        uint32_t numThreads = m_capturedParams.methodInfo.numThreads;
        if (numThreads > maxThreads) numThreads = maxThreads;
        if (numThreads < 1) numThreads = 1;
        m_capturedParams.methodInfo.numThreads = numThreads;

        // Create and prepare threads
        for (uint32_t i = 0; i < numThreads; i++)
        {
            std::shared_ptr<ConvolutionThread> ct = std::make_shared<ConvolutionThread>(
                numThreads, i, m_capturedParams,
                inputBuffer.data(), inputWidth, inputHeight,
                kernelBuffer.data(), kernelWidth, kernelHeight);

            std::vector<float>& threadBuffer = ct->getBuffer();
            threadBuffer.resize(inputBufferSize);
            for (uint32_t j = 0; j < inputBufferSize; j++)
                threadBuffer[j] = (j % 4 == 3) ? 1.0f : 0.0f;

            m_threads.push_back(ct);
        }

        // Start the threads
        for (auto& ct : m_threads)
        {
            std::shared_ptr<std::jthread> t = std::make_shared<std::jthread>(
                [ct]()
                {
                    ct->start();
                }
            );
            ct->setThread(t);
        }

        // Wait for the threads
        std::chrono::time_point<std::chrono::system_clock> lastProgTime = std::chrono::system_clock::now();
        while (true)
        {
            uint32_t numThreadsDone = 0;
            ConvolutionThread* ct;
            ConvolutionThreadStats* stats;

            for (auto& ct : m_threads)
            {
                stats = ct->getStats();
                if (stats->state == ConvolutionThreadState::Done)
                    numThreadsDone++;
            }

            // Keep in mind that the threads will be "Done" when mustCancel is true
            if (numThreadsDone >= numThreads)
                break;

            // Take a snapshot of the current progress

            bool mustUpdateProg =
                (!m_status.mustCancel())
                && (getElapsedMs(lastProgTime) > CONV_PROG_TIMESTEP)
                && (numThreadsDone < numThreads);

            if (mustUpdateProg)
            {
                std::vector<float> progBuffer;
                progBuffer.resize(inputBufferSize);
                for (uint32_t i = 0; i < inputBufferSize; i++)
                    progBuffer[i] = (i % 4 == 3) ? 1.0f : 0.0f;

                for (auto& ct : m_threads)
                {
                    std::vector<float>& currentBuffer = ct->getBuffer();
                    for (uint32_t y = 0; y < inputHeight; y++)
                    {
                        for (uint32_t x = 0; x < inputWidth; x++)
                        {
                            uint32_t redIndex = (y * inputWidth + x) * 4;
                            progBuffer[redIndex + 0] += currentBuffer[redIndex + 0] * CONV_MULTIPLIER;
                            progBuffer[redIndex + 1] += currentBuffer[redIndex + 1] * CONV_MULTIPLIER;
                            progBuffer[redIndex + 2] += currentBuffer[redIndex + 2] * CONV_MULTIPLIER;
                        }
                    }
                }

                // Update Conv. Mix
                {
                    std::scoped_lock lock(*m_imgConvMix);
                    m_imgConvMix->resize(inputWidth, inputHeight, false);
                    float* convBuffer = m_imgConvMix->getImageData();
                    std::copy(progBuffer.data(), progBuffer.data() + inputBufferSize, convBuffer);
                }
                m_imgConvMix->moveToGPU();

                lastProgTime = std::chrono::system_clock::now();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(WAIT_TIMESTEP_SHORT));
        }

        // Join the threads
        for (auto& ct : m_threads)
            threadJoin(ct->getThread().get());

        // Clear input buffers
        clearVector(inputBuffer);
        clearVector(kernelBuffer);

        // Update the output image
        {
            std::scoped_lock lock(m_imgOutput);
            m_imgOutput.resize(inputWidth, inputHeight, false);
            m_imgOutput.fill(std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f }, false);
            float* convBuffer = m_imgOutput.getImageData();

            // Add the buffers from each thread
            for (auto& ct : m_threads)
            {
                std::vector<float>& currentBuffer = ct->getBuffer();
                for (uint32_t y = 0; y < inputHeight; y++)
                {
                    for (uint32_t x = 0; x < inputWidth; x++)
                    {
                        uint32_t redIndex = (y * inputWidth + x) * 4;
                        convBuffer[redIndex + 0] += currentBuffer[redIndex + 0] * CONV_MULTIPLIER;
                        convBuffer[redIndex + 1] += currentBuffer[redIndex + 1] * CONV_MULTIPLIER;
                        convBuffer[redIndex + 2] += currentBuffer[redIndex + 2] * CONV_MULTIPLIER;
                    }
                }
            }
        }

        // Clean up
        clearVector(m_threads);
    }

    void Convolution::convNaiveGPU(
        std::vector<float>& kernelBuffer,
        uint32_t kernelWidth,
        uint32_t kernelHeight,
        std::vector<float>& inputBuffer,
        uint32_t inputWidth,
        uint32_t inputHeight,
        uint32_t inputBufferSize)
    {
        // Clamp numChunks
        if (m_capturedParams.methodInfo.numChunks < 1) m_capturedParams.methodInfo.numChunks = 1;
        if (m_capturedParams.methodInfo.numChunks > CONV_MAX_CHUNKS) m_capturedParams.methodInfo.numChunks = CONV_MAX_CHUNKS;
        uint32_t numChunks = m_capturedParams.methodInfo.numChunks;

        // Clamp chunkSleep
        if (m_capturedParams.methodInfo.chunkSleep > CONV_MAX_SLEEP) m_capturedParams.methodInfo.chunkSleep = CONV_MAX_SLEEP;
        uint32_t chunkSleep = m_capturedParams.methodInfo.chunkSleep;

        // Create GpuHelper instance
        GpuHelper gpuHelper;
        std::string inpFilename = gpuHelper.getInpFilename();
        std::string outFilename = gpuHelper.getOutFilename();

        std::string statFilename = inpFilename + "stat";
        HANDLE statMutex = NULL;

        try
        {
            // Prepare input data
            BinaryConvNaiveGpuInput binInput;
            binInput.numChunks = numChunks;
            binInput.chunkSleep = chunkSleep;
            binInput.cp_kernelCenterX = m_capturedParams.kernelCenterX;
            binInput.cp_kernelCenterY = m_capturedParams.kernelCenterY;
            binInput.cp_convThreshold = m_capturedParams.threshold;
            binInput.cp_convKnee = m_capturedParams.knee;
            binInput.convMultiplier = CONV_MULTIPLIER;
            binInput.inputWidth = inputWidth;
            binInput.inputHeight = inputHeight;
            binInput.kernelWidth = kernelWidth;
            binInput.kernelHeight = kernelHeight;
            binInput.inputBuffer = inputBuffer;
            binInput.kernelBuffer = kernelBuffer;

            // Mutex for IO operations on the stat file

            std::string statMutexName = "GlobalRbStatMutex" + std::to_string(gpuHelper.getRandomNumber());
            binInput.statMutexName = statMutexName;

            statMutex = createMutex(statMutexName);
            if (statMutex == NULL)
                throw std::exception(
                    strFormat("Mutex \"%s\" could not be created.", statMutexName.c_str()).c_str()
                );

            // Create the input file
            std::ofstream inpFile;
            inpFile.open(inpFilename, std::ofstream::out | std::ofstream::binary | std::ofstream::trunc);
            if (!inpFile.is_open())
                throw std::exception(
                    strFormat("Input file \"%s\" could not be created/opened.", inpFilename.c_str()).c_str()
                );

            // Write operation type
            uint32_t opType = (uint32_t)GpuHelperOperationType::ConvNaive;
            stmWriteScalar(inpFile, opType);

            // Write input data
            binInput.writeTo(inpFile);

            // Close the input file
            inpFile.flush();
            inpFile.close();

            // Execute GPU Helper
            gpuHelper.run();

            // Wait for the output file to be created
            gpuHelper.waitForOutput([this]() { return m_status.mustCancel(); });

            // Wait for the GPU Helper to finish its job
            auto lastProgTime = std::chrono::system_clock::now();
            while (gpuHelper.isRunning())
            {
                if (m_status.mustCancel())
                    throw std::exception();

                // Read the stat file

                bool mustReadStat =
                    !(m_status.mustCancel())
                    && std::filesystem::exists(statFilename)
                    && (getElapsedMs(lastProgTime) > CONV_PROG_TIMESTEP);

                if (mustReadStat)
                {
                    BinaryConvNaiveGpuStat binStat;
                    waitForMutex(statMutex);

                    // Open and read the stat file
                    std::ifstream statFile;
                    statFile.open(statFilename, std::ifstream::in | std::ifstream::binary);
                    if (statFile.is_open())
                    {
                        statFile.seekg(std::ifstream::beg);
                        try
                        {
                            binStat.readFrom(statFile);

                            // If numChunksDone has increased
                            if (binStat.numChunksDone > m_status.getNumChunksDone())
                            {
                                m_status.setNumChunksDone(binStat.numChunksDone);

                                // Update Conv. Mix
                                if (binStat.buffer.size() == (inputWidth * inputHeight * 4))
                                {
                                    std::scoped_lock lock(*m_imgConvMix);
                                    m_imgConvMix->resize(inputWidth, inputHeight, false);
                                    float* convBuffer = m_imgConvMix->getImageData();
                                    std::copy(binStat.buffer.data(), binStat.buffer.data() + binStat.buffer.size(), convBuffer);
                                }
                                m_imgConvMix->moveToGPU();
                            }
                        }
                        catch (const std::exception& e)
                        {
                            printError(__FUNCTION__, "stat", e.what());
                        }
                        statFile.close();
                    }
                    else
                    {
                        printError(__FUNCTION__, "stat", strFormat("Stat file \"%s\" could not be opened.", statFilename.c_str()));
                    }

                    releaseMutex(statMutex);
                    lastProgTime = std::chrono::system_clock::now();
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(WAIT_TIMESTEP_SHORT));
            }

            // No longer need the mutex
            closeMutex(statMutex);

            // Open the output file
            std::ifstream outFile;
            outFile.open(outFilename, std::ifstream::in | std::ifstream::binary);
            if (!outFile.is_open())
                throw std::exception(
                    strFormat("Output file \"%s\" could not be opened.", outFilename.c_str()).c_str()
                );
            else
                outFile.seekg(std::ifstream::beg);

            // Parse the output file
            BinaryConvNaiveGpuOutput binOutput;
            binOutput.readFrom(outFile);
            if (binOutput.status == 1)
            {
                // Update the output image

                std::scoped_lock lock(m_imgOutput);
                m_imgOutput.resize(inputWidth, inputHeight, false);
                float* convBuffer = m_imgOutput.getImageData();
                uint32_t convBufferSize = m_imgOutput.getImageDataSize();

                if (convBufferSize == binOutput.buffer.size())
                {
                    std::copy(binOutput.buffer.data(), binOutput.buffer.data() + binOutput.buffer.size(), convBuffer);
                }
                else
                {
                    m_status.setError(strFormat(
                        "Output buffer size (%u) does not match the input buffer size (%u).",
                        binOutput.buffer.size(), convBufferSize
                    ));
                }
            }
            else
            {
                m_status.setError(binOutput.error);
            }

            // Close the output file
            outFile.close();
        }
        catch (const std::exception& e)
        {
            m_status.setError(e.what());
        }

        // Clean up
        gpuHelper.cleanUp();
        closeMutex(statMutex);
        if (GPU_HELPER_DELETE_TEMP)
        {
            deleteFile(statFilename);
        }
    }

    void drawRect(CmImage* image, int rx, int ry, int rw, int rh)
    {
        std::scoped_lock lock(*image);
        float* buffer = image->getImageData();
        uint32_t imageWidth = image->getWidth();
        uint32_t imageHeight = image->getHeight();

        int rx2 = rx + rw;
        int ry2 = ry + rh;

        uint32_t redIndex;
        for (int y = ry; y < ry2; y++)
        {
            if (checkBounds(rx, y, imageWidth, imageHeight))
            {
                redIndex = (y * imageWidth + rx) * 4;
                buffer[redIndex + 0] = 0;
                buffer[redIndex + 1] = 0;
                buffer[redIndex + 2] = 1;
                buffer[redIndex + 3] = 1;
            }

            if (checkBounds(rx2, y, imageWidth, imageHeight))
            {
                redIndex = (y * imageWidth + rx2) * 4;
                buffer[redIndex + 0] = 0;
                buffer[redIndex + 1] = 0;
                buffer[redIndex + 2] = 1;
                buffer[redIndex + 3] = 1;
            }
        }
        for (int x = rx; x < rx2; x++)
        {
            if (checkBounds(x, ry, imageWidth, imageHeight))
            {
                redIndex = (ry * imageWidth + x) * 4;
                buffer[redIndex + 0] = 0;
                buffer[redIndex + 1] = 0;
                buffer[redIndex + 2] = 1;
                buffer[redIndex + 3] = 1;
            }

            if (checkBounds(x, ry2, imageWidth, imageHeight))
            {
                redIndex = (ry2 * imageWidth + x) * 4;
                buffer[redIndex + 0] = 0;
                buffer[redIndex + 1] = 0;
                buffer[redIndex + 2] = 1;
                buffer[redIndex + 3] = 1;
            }
        }
    }

    void fillRect(CmImage* image, int rx, int ry, int rw, int rh)
    {
        std::scoped_lock lock(*image);
        float* buffer = image->getImageData();
        uint32_t imageWidth = image->getWidth();
        uint32_t imageHeight = image->getHeight();

        int rx2 = rx + rw;
        int ry2 = ry + rh;

        for (int y = ry; y < ry2; y++)
        {
            for (int x = rx; x < rx2; x++)
            {
                if (checkBounds(x, y, imageWidth, imageHeight))
                {
                    uint32_t redIndex = (y * imageWidth + x) * 4;
                    buffer[redIndex + 0] = 0;
                    buffer[redIndex + 1] = 0;
                    buffer[redIndex + 2] = 1;
                    buffer[redIndex + 3] = 1;
                }
            }
        }
    }

}
