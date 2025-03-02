#include "CMF.h"

const std::string CMF_DIR = getLocalPath("cmf") + getPathSeparator();
const std::string CMF_DEFAULT_TABLE = "CIE 1931 2-deg";

#pragma region CmfTable

CmfTable::CmfTable(std::string filename)
{
    try
    {
        if (!std::filesystem::exists(filename))
        {
            filename = CMF_DIR + filename;
            if (!std::filesystem::exists(filename))
                throw std::exception("File does not exist.");
        }

        // Read the CSV file
        rapidcsv::Document doc(filename, rapidcsv::LabelParams(-1, -1));

        if (doc.GetColumnCount() < 4)
            throw std::exception("At least 4 columns are required (wavelength, X, Y, Z).");

        // Get the wavelength info
        std::vector<float> wavelengths = doc.GetColumn<float>(0);

        // This is kind of arbitrary but what would you use
        // a CMF table with less than 10 entries for
        if (wavelengths.size() < 10)
            throw std::exception(
                strFormat("At least 10 entries are needed. (%u)", wavelengths.size()).c_str()
            );

        m_count = wavelengths.size();
        m_start = wavelengths[0];
        m_end = wavelengths[wavelengths.size() - 1];
        m_step = wavelengths[1] - wavelengths[0];

        // start + (step * (count - 1)) == end
        if (fabsf((m_start + (m_step * (float)(m_count - 1))) - m_end) >= m_step)
            throw std::exception(strFormat(
                "Wavelength entries must be equally distanced from each other. "
                "(%u, %.3f, %.3f, %.3f)",
                m_count,
                m_start,
                m_end,
                m_step
            ).c_str());

        // Get XYZ values
        m_valuesX = doc.GetColumn<float>(1);
        m_valuesY = doc.GetColumn<float>(2);
        m_valuesZ = doc.GetColumn<float>(3);

        // Check if they all have the same number of entries
        // Pretty long conditions in this function, eh
        if (!((m_valuesX.size() == m_count) &&
            (m_valuesY.size() == m_count) &&
            (m_valuesZ.size() == m_count)))
        {
            throw std::exception(strFormat(
                "The first 4 columns must have the same number of entries. "
                "(%u, %u, %u, %u)",
                m_count,
                m_valuesX.size(),
                m_valuesY.size(),
                m_valuesZ.size()
            ).c_str());
        }
    }
    catch (const std::exception& e)
    {
        throw std::exception(makeError(__FUNCTION__, "", e.what(), true).c_str());
    }
}

size_t CmfTable::getCount() const
{
    return m_count;
}

float CmfTable::getStart() const
{
    return m_start;
}

float CmfTable::getEnd() const
{
    return m_end;
}

float CmfTable::getRange() const
{
    return (m_end - m_start);
}

float CmfTable::getStep() const
{
    return m_step;
}

std::array<float, 3> CmfTable::sample(float wavelength) const
{
    // Out of range values

    if (wavelength < m_start)
        return { m_valuesX[0], m_valuesY[0], m_valuesZ[0] };

    if (wavelength > m_end)
        return { m_valuesX[m_count - 1], m_valuesY[m_count - 1], m_valuesZ[m_count - 1] };

    // How far along the range
    float pos = (wavelength - m_start) / (m_end - m_start);

    // Fuzzy index
    float fidx = pos * (float)(m_count - 1);

    // Integer index
    size_t idx = (size_t)floorf(fidx);

    // Fractional part
    float offset = fidx - (float)idx;

    // If it's the last entry
    if (idx >= (m_count - 1))
        return { m_valuesX[m_count - 1], m_valuesY[m_count - 1], m_valuesZ[m_count - 1] };

    // Sample linearly from idx to idx+1
    float smpX = lerp(m_valuesX[idx], m_valuesX[idx + 1], offset);
    float smpY = lerp(m_valuesY[idx], m_valuesY[idx + 1], offset);
    float smpZ = lerp(m_valuesZ[idx], m_valuesZ[idx + 1], offset);

    return { smpX, smpY, smpZ };
}

void CmfTable::sample(size_t numSamples, std::vector<float>& outSamples) const
{
    outSamples.clear();
    outSamples.reserve(numSamples * 3);

    float range = getRange();
    float pos, wl;
    std::array<float, 3> smp;

    for (size_t i = 0; i < numSamples; i++)
    {
        pos = float(i) / (float)(numSamples - 1);
        wl = m_start + (pos * range);

        smp = sample(wl);
        outSamples.push_back(smp[0]);
        outSamples.push_back(smp[1]);
        outSamples.push_back(smp[2]);
    }
}

void CmfTable::sampleRGB(size_t numSamples, std::vector<float>& outSamples) const
{
    sample(numSamples, outSamples);

    std::string stage = "";
    try
    {
        CMS::ensureOK();
        CmXYZ::ensureOK();

        OCIO::ConstConfigRcPtr userConfig = CMS::getConfig();
        OCIO::ConstConfigRcPtr internalConfig = CMS::getInternalConfig();

        XyzConversionInfo info = CmXYZ::getConversionInfo();
        if (info.method == XyzConversionMethod::None)
            throw std::exception("An XYZ conversion method was not specified.");

        OCIO::PackedImageDesc img(
            outSamples.data(),
            numSamples,
            1,
            OCIO::ChannelOrdering::CHANNEL_ORDERING_RGB,
            OCIO::BitDepth::BIT_DEPTH_F32,
            4,                    // 4 bytes to go to the next color channel
            3 * 4,                // 3 color channels * 4 bytes per channel (till the next pixel)
            numSamples * 3 * 4);  // width * 3 channels * 4 bytes (till the next row)

        if (info.method == XyzConversionMethod::UserConfig)
        {
            // Transform: XYZ -> Working Space (user config)
            stage = "Transform (UserConfig)";
            {
                OCIO::ColorSpaceTransformRcPtr transform = OCIO::ColorSpaceTransform::Create();
                transform->setSrc(info.userSpace.c_str());
                transform->setDst(OCIO::ROLE_SCENE_LINEAR);

                OCIO::ConstProcessorRcPtr proc = userConfig->getProcessor(transform);
                OCIO::ConstCPUProcessorRcPtr cpuProc = proc->getDefaultCPUProcessor();
                cpuProc->apply(img);
            }
        }
        else if (info.method == XyzConversionMethod::CommonSpace)
        {
            // Transform 1: XYZ -> Common Space (internal config)
            stage = "Transform 1 (CommonSpace)";
            {
                OCIO::ColorSpaceTransformRcPtr transform = OCIO::ColorSpaceTransform::Create();
                transform->setSrc(CMS::getInternalXyzSpace().c_str());
                transform->setDst(info.commonInternal.c_str());

                OCIO::ConstProcessorRcPtr proc = internalConfig->getProcessor(transform);
                OCIO::ConstCPUProcessorRcPtr cpuProc = proc->getDefaultCPUProcessor();
                cpuProc->apply(img);
            }

            // Transform 2: Common Space -> Working Space (user config)
            stage = "Transform 2 (CommonSpace)";
            {
                OCIO::ColorSpaceTransformRcPtr transform = OCIO::ColorSpaceTransform::Create();
                transform->setSrc(info.commonUser.c_str());
                transform->setDst(OCIO::ROLE_SCENE_LINEAR);

                OCIO::ConstProcessorRcPtr proc = userConfig->getProcessor(transform);
                OCIO::ConstCPUProcessorRcPtr cpuProc = proc->getDefaultCPUProcessor();
                cpuProc->apply(img);
            }
        }

        // Eliminate negative values
        {
            float minV;
            size_t redIndex;
            for (size_t i = 0; i < numSamples; i++)
            {
                redIndex = i * 3;

                minV = fminf(fminf(outSamples[redIndex], outSamples[redIndex + 1]), outSamples[redIndex + 2]);
                if (minV < 0.0f)
                {
                    outSamples[redIndex + 0] -= minV;
                    outSamples[redIndex + 1] -= minV;
                    outSamples[redIndex + 2] -= minV;
                }
            }
        }
        for (auto& v : outSamples)
            v = fmaxf(v, 0.0f);
    }
    catch (std::exception& e)
    {
        throw std::exception(makeError(__FUNCTION__, stage, e.what(), true).c_str());
    }
}

CmfTableInfo::CmfTableInfo(const std::string& name, const std::string& path)
    : name(name), path(path)
{}

#pragma endregion

#pragma region CMF

CMF::CmfVars* CMF::S_VARS = nullptr;
BaseStatus CMF::S_STATUS;

void CMF::retrieveTables()
{
    S_VARS->tables.clear();

    if (!(std::filesystem::exists(CMF_DIR) && std::filesystem::is_directory(CMF_DIR)))
        throw std::exception(strFormat("\"%s\" was not found.", CMF_DIR.c_str()).c_str());

    for (const auto& entry : std::filesystem::directory_iterator(CMF_DIR))
    {
        if (!entry.is_directory())
        {
            if (strLowercase(entry.path().extension().string()) == ".csv")
            {
                std::string tableName = entry.path().filename().replace_extension().string();
                std::string tablePath = std::filesystem::absolute(entry.path()).string();
                S_VARS->tables.push_back(CmfTableInfo(tableName, tablePath));
            }
        }
    }
}

bool CMF::init()
{
    S_VARS = new CmfVars();
    try
    {
        retrieveTables();
    }
    catch (const std::exception& e)
    {
        S_STATUS.setError(makeError(__FUNCTION__, "", e.what(), true));
    }

    // Load the default table if it is found
    if (S_STATUS.isOK())
    {
        for (const auto& tbl : S_VARS->tables)
        {
            if (tbl.name == CMF_DEFAULT_TABLE)
            {
                setActiveTable(tbl);
                break;
            }
        }

        // If the default wasn't found, load the first one available
        if ((!CMF::hasTable()) && (S_VARS->tables.size() > 0))
            setActiveTable(S_VARS->tables[0]);
    }

    return S_STATUS.isOK();
}

void CMF::cleanUp()
{
    DELPTR(S_VARS);
}

const std::vector<CmfTableInfo>& CMF::getTables()
{
    return S_VARS->tables;
}

std::shared_ptr<CmfTable> CMF::getActiveTable()
{
    return S_VARS->activeTable;
}

const CmfTableInfo& CMF::getActiveTableInfo()
{
    return S_VARS->activeTableInfo;
}

const std::string& CMF::getActiveTableDetails()
{
    return S_VARS->activeTableDetails;
}

void CMF::setActiveTable(const CmfTableInfo& tableInfo)
{
    S_STATUS.reset();

    S_VARS->activeTable = nullptr;
    try
    {
        if (!tableInfo.path.empty())
        {
            S_VARS->activeTable = std::make_shared<CmfTable>(tableInfo.path);
            S_VARS->activeTableInfo = tableInfo;

            S_VARS->activeTableDetails = strFormat(
                "%s\n"
                "Count: %u\n"
                "Range: %.3f - %.3f\n"
                "Step: %.3f",
                S_VARS->activeTableInfo.name.c_str(),
                S_VARS->activeTable->getCount(),
                S_VARS->activeTable->getStart(),
                S_VARS->activeTable->getEnd(),
                S_VARS->activeTable->getStep()
            );
        }
    }
    catch (const std::exception& e)
    {
        S_VARS->activeTable = nullptr;
        S_STATUS.setError(e.what());
    }
}

bool CMF::hasTable()
{
    return (S_VARS->activeTable.get() != nullptr);
}

const BaseStatus& CMF::getStatus()
{
    return S_STATUS;
}

#pragma endregion
