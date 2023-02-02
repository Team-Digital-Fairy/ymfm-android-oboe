#include <jni.h>

// ymfm thing
#include "ymfm.h"
#include "ymfm_misc.h"
#include "ymfm_opl.h"
#include "ymfm_opm.h"
#include "ymfm_opn.h"

// Oboe thing
#include <oboe/Oboe.h>
using namespace oboe;
#include "handler.h"

#include <android/log.h>

#define LOG_TAG "JNI-ymfm"

#define LOG_E(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,  __VA_ARGS__);
#define LOG_I(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG,  __VA_ARGS__);
#define LOG_D(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG,  __VA_ARGS__);

static YmfmHandler hdr;

using emulated_time = int64_t;

enum chip_type
{
    CHIP_YM2149,
    CHIP_YM2151,
    CHIP_YM2203,
    CHIP_YM2413,
    CHIP_YM2608,
    CHIP_YM2610,
    CHIP_YM2612,
    CHIP_YM3526,
    CHIP_Y8950,
    CHIP_YM3812,
    CHIP_YMF262,
    CHIP_YMF278B,
    CHIP_TYPES
};

class vgm_chip_base
{
public:
    // construction
    vgm_chip_base(uint32_t clock, chip_type type, char const *name) :
            m_type(type),
            m_name(name)
    {
    }

    // destruction
    virtual ~vgm_chip_base() = default;

    // simple getters
    chip_type type() const { return m_type; }
    virtual uint32_t sample_rate() const = 0;

    // required methods for derived classes to implement
    virtual void write(uint32_t reg, uint8_t data) = 0;
    virtual void generate(emulated_time output_start, emulated_time output_step, int32_t *buffer) = 0;

    // write data to the ADPCM-A buffer
    void write_data(ymfm::access_class type, uint32_t base, uint32_t length, uint8_t const *src)
    {
        uint32_t end = base + length;
        if (end > m_data[type].size())
            m_data[type].resize(end);
        memcpy(&m_data[type][base], src, length);
    }

    // seek within the PCM stream
    void seek_pcm(uint32_t pos) { m_pcm_offset = pos; }
    uint8_t read_pcm() { auto &pcm = m_data[ymfm::ACCESS_PCM]; return (m_pcm_offset < pcm.size()) ? pcm[m_pcm_offset++] : 0; }

protected:
    // internal state
    chip_type m_type;
    std::string m_name;
    std::vector<uint8_t> m_data[ymfm::ACCESS_CLASSES];
    uint32_t m_pcm_offset;

};

template<typename ChipType>
class vgm_chip : public vgm_chip_base, public ymfm::ymfm_interface
{
public:
    // construction
    vgm_chip(uint32_t clock, chip_type type, char const *name) :
            vgm_chip_base(clock, type, name),
            m_chip(*this),
            m_clock(clock),
            m_clocks(0),
            m_step(0x100000000ull / m_chip.sample_rate(clock)),
            m_pos(0)
    {
        m_chip.reset();

#ifdef EXTRA_CLOCKS
        for (int clock = 0; clock < EXTRA_CLOCKS; clock++)
            m_chip.generate(&m_output);
#endif

#if (RUN_NUKED_OPN2)
        if (type == CHIP_YM2612)
		{
			m_external = new nuked::ym3438_t;
			nuked::OPN2_SetChipType(nuked::ym3438_mode_ym2612);
			nuked::OPN2_Reset(m_external);
			nuked::Bit16s buffer[2];
			for (int clocks = 0; clocks < 24 * EXTRA_CLOCKS; clocks++)
				nuked::OPN2_Clock(m_external, buffer);
		}
#endif
    }

    virtual uint32_t sample_rate() const override
    {
        return m_chip.sample_rate(m_clock);
    }

    // handle a register write: just queue for now
    virtual void write(uint32_t reg, uint8_t data) override
    {
        m_queue.push_back(std::make_pair(reg, data));
    }

    // generate one output sample of output
    virtual void generate(emulated_time output_start, emulated_time output_step, int32_t *buffer) override
    {
        uint32_t addr1 = 0xffff, addr2 = 0xffff;
        uint8_t data1 = 0, data2 = 0;

        // see if there is data to be written; if so, extract it and dequeue
        if (!m_queue.empty())
        {
            auto front = m_queue.front();
            addr1 = 0 + 2 * ((front.first >> 8) & 3);
            data1 = front.first & 0xff;
            addr2 = addr1 + ((m_type == CHIP_YM2149) ? 2 : 1);
            data2 = front.second;
            m_queue.erase(m_queue.begin());
        }

        // write to the chip
        if (addr1 != 0xffff)
        {
            //if (LOG_WRITES)
            //    printf("%10.5f: %s %03X=%02X\n", double(output_start) / double(1LL << 32), m_name.c_str(), data1 + 0x100 * (addr1/2), data2);
            m_chip.write(addr1, data1);
            m_chip.write(addr2, data2);
        }

        // generate at the appropriate sample rate
//		nuked::s_log_envelopes = (output_start >= (22ll << 32) && output_start < (24ll << 32));
        for ( ; m_pos <= output_start; m_pos += m_step)
        {
            m_chip.generate(&m_output);

#if (CAPTURE_NATIVE)
            // if capturing native, append each generated sample
			m_native_data.push_back(m_output.data[0]);
			m_native_data.push_back(m_output.data[ChipType::OUTPUTS > 1 ? 1 : 0]);
#endif

#if (RUN_NUKED_OPN2)
            // if running nuked, capture its output as well
			if (m_external != nullptr)
			{
				int32_t sum[2] = { 0 };
				if (addr1 != 0xffff)
					nuked::OPN2_Write(m_external, addr1, data1);
				nuked::Bit16s buffer[2];
				for (int clocks = 0; clocks < 12; clocks++)
				{
					nuked::OPN2_Clock(m_external, buffer);
					sum[0] += buffer[0];
					sum[1] += buffer[1];
				}
				if (addr2 != 0xffff)
					nuked::OPN2_Write(m_external, addr2, data2);
				for (int clocks = 0; clocks < 12; clocks++)
				{
					nuked::OPN2_Clock(m_external, buffer);
					sum[0] += buffer[0];
					sum[1] += buffer[1];
				}
				addr1 = addr2 = 0xffff;
				m_nuked_data.push_back(sum[0] / 24);
				m_nuked_data.push_back(sum[1] / 24);
			}
#endif
        }

        // add the final result to the buffer
        if (m_type == CHIP_YM2203)
        {
            int32_t out0 = m_output.data[0];
            int32_t out1 = m_output.data[1 % ChipType::OUTPUTS];
            int32_t out2 = m_output.data[2 % ChipType::OUTPUTS];
            int32_t out3 = m_output.data[3 % ChipType::OUTPUTS];
            *buffer++ += out0 + out1 + out2 + out3;
            *buffer++ += out0 + out1 + out2 + out3;
        }
        else if (m_type == CHIP_YM2608 || m_type == CHIP_YM2610)
        {
            int32_t out0 = m_output.data[0];
            int32_t out1 = m_output.data[1 % ChipType::OUTPUTS];
            int32_t out2 = m_output.data[2 % ChipType::OUTPUTS];
            *buffer++ += out0 + out2;
            *buffer++ += out1 + out2;
        }
        else if (m_type == CHIP_YMF278B)
        {
            *buffer++ += m_output.data[4 % ChipType::OUTPUTS];
            *buffer++ += m_output.data[5 % ChipType::OUTPUTS];
        }
        else if (ChipType::OUTPUTS == 1)
        {
            *buffer++ += m_output.data[0];
            *buffer++ += m_output.data[0];
        }
        else
        {
            *buffer++ += m_output.data[0];
            *buffer++ += m_output.data[1 % ChipType::OUTPUTS];
        }
        m_clocks++;
    }

protected:
    // handle a read from the buffer
    virtual uint8_t ymfm_external_read(ymfm::access_class type, uint32_t offset) override
    {
        auto &data = m_data[type];
        return (offset < data.size()) ? data[offset] : 0;
    }

    // internal state
    ChipType m_chip;
    uint32_t m_clock;
    uint64_t m_clocks;
    typename ChipType::output_data m_output;
    emulated_time m_step;
    emulated_time m_pos;
    std::vector<std::pair<uint32_t, uint8_t>> m_queue;
};

std::vector<std::unique_ptr<vgm_chip_base>> active_chips;

uint32_t parse_uint32(std::vector<uint8_t> &buffer, uint32_t &offset)
{
    uint32_t result = buffer[offset++];
    result |= buffer[offset++] << 8;
    result |= buffer[offset++] << 16;
    result |= buffer[offset++] << 24;
    return result;
}


template<typename ChipType>
void add_chips(uint32_t clock, chip_type type, char const *chipname)
{
    uint32_t clockval = clock & 0x3fffffff;
    int numchips = (clock & 0x40000000) ? 2 : 1;
    printf("Adding %s%s @ %dHz\n", (numchips == 2) ? "2 x " : "", chipname, clockval);
    for (int index = 0; index < numchips; index++)
    {
        char name[100];
        sprintf(name, "%s #%d", chipname, index);
        active_chips.push_back(std::make_unique<vgm_chip<ChipType>>(clockval, type, (numchips == 2) ? name : chipname));
    }

    if (type == CHIP_YM2608)
    {
        FILE *rom = fopen("ym2608_adpcm_rom.bin", "rb");
        if (rom == nullptr)
            fprintf(stderr, "Warning: YM2608 enabled but ym2608_adpcm_rom.bin not found\n");
        else
        {
            fseek(rom, 0, SEEK_END);
            uint32_t size = ftell(rom);
            fseek(rom, 0, SEEK_SET);
            std::vector<uint8_t> temp(size);
            fread(&temp[0], 1, size, rom);
            fclose(rom);
            for (auto &chip : active_chips)
                if (chip->type() == type)
                    chip->write_data(ymfm::ACCESS_ADPCM_A, 0, size, &temp[0]);
        }
    }
}


extern "C"
JNIEXPORT void JNICALL
Java_team_digitalfairy_ymfm_1thing_YmfmInterface_startOboe(JNIEnv *env, jclass clazz) {
    hdr.open();

    add_chips<ymfm::ym2151>(3579545, CHIP_YM2151, "YM2151");


}

// Step 1. load VGM file and prepare YMFM Context
// Step 2. Run down the VGM ticks bound to 44100; yet sound needs to be aligned to 44100
//