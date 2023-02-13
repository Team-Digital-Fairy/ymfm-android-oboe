#include <jni.h>
#include <error.h>
#include <algorithm>

//#include <mutex>
#include <pthread.h>
#include <ctime>
#include <unistd.h>
#include <cmath>
#include <deque>

// ymfm thing
#include "ymfm.h"
#include "ymfm_misc.h"
#include "ymfm_opl.h"
#include "ymfm_opm.h"
#include "ymfm_opn.h"



// Oboe thing
#include <oboe/Oboe.h>
using namespace oboe;
#include "old/handler.h.old"

#include <android/log.h>

#define LOG_TAG "JNI-ymfm"

#define LOG_E(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,  __VA_ARGS__);
#define LOG_I(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG,  __VA_ARGS__);
#define LOG_D(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG,  __VA_ARGS__);

static YmfmHandler hdr;

using emulated_time = uint64_t;

// VGM Datas

uint32_t vgm_data_start;
uint32_t loop_offset;
std::vector<uint8_t> vgm_buffer;


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

        for (int clock = 0; clock < 0; clock++)
            m_chip.generate(&m_output);


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
        m_queue.emplace_back(reg, data);
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
    LOG_I("Adding %s%s @ %dHz\n", (numchips == 2) ? "2 x " : "", chipname, clockval);
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

vgm_chip_base *find_chip(chip_type type, uint8_t index)
{
    for (auto &chip : active_chips)
        if (chip->type() == type && index-- == 0)
            return chip.get();
    return nullptr;
}


void write_chip(chip_type type, uint8_t index, uint32_t reg, uint8_t data)
{
    vgm_chip_base *chip = find_chip(type, index);
    if (chip != nullptr)
        chip->write(reg, data);
}

uint32_t parse_header(std::vector<uint8_t> &buffer) {
    // +00: already checked the ID
    uint32_t offset = 4;

    // +04: parse the size
    uint32_t size = parse_uint32(buffer, offset);
    if (offset - 4 + size > buffer.size()) {
        fprintf(stderr, "Total size for file is too small; file may be truncated\n");
        size = buffer.size() - 4;
    }
    buffer.resize(size + 4);

    // +08: parse the version
    uint32_t version = parse_uint32(buffer, offset);
    LOG_I("VGM Version %08X", version);
    if (version > 0x171)
        fprintf(stderr, "Warning: version > 1.71 detected, some things may not work\n");

    // +0C: SN76489 clock
    uint32_t clock = parse_uint32(buffer, offset);
    if (clock != 0)
        fprintf(stderr, "Warning: clock for SN76489 specified (%d), but not supported\n", clock);

    // +10: YM2413 clock
    clock = parse_uint32(buffer, offset);
    if (clock != 0)
        add_chips<ymfm::ym2413>(clock, CHIP_YM2413, "YM2413");

    // +14: GD3 offset
    uint32_t dummy = parse_uint32(buffer, offset);

    // +18: Total # samples
    dummy = parse_uint32(buffer, offset);

    // +1C: Loop offset
    loop_offset = parse_uint32(buffer, offset);

    // +20: Loop # samples
    dummy = parse_uint32(buffer, offset);

    // +24: Rate
    dummy = parse_uint32(buffer, offset);

    // +28: SN76489 feedback / SN76489 shift register width / SN76489 Flags
    dummy = parse_uint32(buffer, offset);

    // +2C: YM2612 clock
    clock = parse_uint32(buffer, offset);
    if (version >= 0x110 && clock != 0)
        add_chips<ymfm::ym2612>(clock, CHIP_YM2612, "YM2612");

    // +30: YM2151 clock
    clock = parse_uint32(buffer, offset);
    if (version >= 0x110 && clock != 0)
        add_chips<ymfm::ym2151>(clock, CHIP_YM2151, "YM2151");

    // +34: VGM data offset
    uint32_t data_start = parse_uint32(buffer, offset);
    data_start += offset - 4;
    if (version < 0x150)
        data_start = 0x40;

    // +38: Sega PCM clock
    clock = parse_uint32(buffer, offset);
    if (version >= 0x151 && clock != 0)
        fprintf(stderr, "Warning: clock for Sega PCM specified, but not supported\n");

    // +3C: Sega PCM interface register
    dummy = parse_uint32(buffer, offset);

    // +40: RF5C68 clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x151 && clock != 0)
        fprintf(stderr, "Warning: clock for RF5C68 specified, but not supported\n");

    // +44: YM2203 clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x151 && clock != 0)
        add_chips<ymfm::ym2203>(clock, CHIP_YM2203, "YM2203");

    // +48: YM2608 clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x151 && clock != 0)
        add_chips<ymfm::ym2608>(clock, CHIP_YM2608, "YM2608");

    // +4C: YM2610/2610B clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x151 && clock != 0) {
        if (clock & 0x80000000)
            add_chips<ymfm::ym2610b>(clock, CHIP_YM2610, "YM2610B");
        else
            add_chips<ymfm::ym2610>(clock, CHIP_YM2610, "YM2610");
    }

    // +50: YM3812 clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x151 && clock != 0)
        add_chips<ymfm::ym3812>(clock, CHIP_YM3812, "YM3812");

    // +54: YM3526 clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x151 && clock != 0)
        add_chips<ymfm::ym3526>(clock, CHIP_YM3526, "YM3526");

    // +58: Y8950 clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x151 && clock != 0)
        add_chips<ymfm::y8950>(clock, CHIP_Y8950, "Y8950");

    // +5C: YMF262 clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x151 && clock != 0)
        add_chips<ymfm::ymf262>(clock, CHIP_YMF262, "YMF262");

    // +60: YMF278B clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x151 && clock != 0)
        add_chips<ymfm::ymf278b>(clock, CHIP_YMF278B, "YMF278B");

    // +64: YMF271 clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x151 && clock != 0)
        fprintf(stderr, "Warning: clock for YMF271 specified, but not supported\n");

    // +68: YMF280B clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x151 && clock != 0)
        fprintf(stderr, "Warning: clock for YMF280B specified, but not supported\n");

    // +6C: RF5C164 clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x151 && clock != 0)
        fprintf(stderr, "Warning: clock for RF5C164 specified, but not supported\n");

    // +70: PWM clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x151 && clock != 0)
        fprintf(stderr, "Warning: clock for PWM specified, but not supported\n");

    // +74: AY8910 clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x151 && clock != 0) {
        fprintf(stderr, "Warning: clock for AY8910 specified, substituting YM2149\n");
        add_chips<ymfm::ym2149>(clock, CHIP_YM2149, "YM2149");
    }

    // +78: AY8910 flags
    if (offset + 4 > data_start)
        return data_start;
    dummy = parse_uint32(buffer, offset);

    // +7C: volume / loop info
    if (offset + 4 > data_start)
        return data_start;
    dummy = parse_uint32(buffer, offset);
    if ((dummy & 0xff) != 0)
        printf("Volume modifier: %02X (=%d)\n", dummy & 0xff,
               int(pow(2, double(dummy & 0xff) / 0x20)));

    // +80: GameBoy DMG clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x161 && clock != 0)
        fprintf(stderr, "Warning: clock for GameBoy DMG specified, but not supported\n");

    // +84: NES APU clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x161 && clock != 0)
        fprintf(stderr, "Warning: clock for NES APU specified, but not supported\n");

    // +88: MultiPCM clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x161 && clock != 0)
        fprintf(stderr, "Warning: clock for MultiPCM specified, but not supported\n");

    // +8C: uPD7759 clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x161 && clock != 0)
        fprintf(stderr, "Warning: clock for uPD7759 specified, but not supported\n");

    // +90: OKIM6258 clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x161 && clock != 0)
        fprintf(stderr, "Warning: clock for OKIM6258 specified, but not supported\n");

    // +94: OKIM6258 Flags / K054539 Flags / C140 Chip Type / reserved
    if (offset + 4 > data_start)
        return data_start;
    dummy = parse_uint32(buffer, offset);

    // +98: OKIM6295 clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x161 && clock != 0)
        fprintf(stderr, "Warning: clock for OKIM6295 specified, but not supported\n");

    // +9C: K051649 clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x161 && clock != 0)
        fprintf(stderr, "Warning: clock for K051649 specified, but not supported\n");

    // +A0: K054539 clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x161 && clock != 0)
        fprintf(stderr, "Warning: clock for K054539 specified, but not supported\n");

    // +A4: HuC6280 clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x161 && clock != 0)
        fprintf(stderr, "Warning: clock for HuC6280 specified, but not supported\n");

    // +A8: C140 clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x161 && clock != 0)
        fprintf(stderr, "Warning: clock for C140 specified, but not supported\n");

    // +AC: K053260 clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x161 && clock != 0)
        fprintf(stderr, "Warning: clock for K053260 specified, but not supported\n");

    // +B0: Pokey clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x161 && clock != 0)
        fprintf(stderr, "Warning: clock for Pokey specified, but not supported\n");

    // +B4: QSound clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x161 && clock != 0)
        fprintf(stderr, "Warning: clock for QSound specified, but not supported\n");

    // +B8: SCSP clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x171 && clock != 0)
        fprintf(stderr, "Warning: clock for SCSP specified, but not supported\n");

    // +BC: extra header offset
    if (offset + 4 > data_start)
        return data_start;
    uint32_t extra_header = parse_uint32(buffer, offset);

    // +C0: WonderSwan clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x171 && clock != 0)
        fprintf(stderr, "Warning: clock for WonderSwan specified, but not supported\n");

    // +C4: VSU clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x171 && clock != 0)
        fprintf(stderr, "Warning: clock for VSU specified, but not supported\n");

    // +C8: SAA1099 clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x171 && clock != 0)
        fprintf(stderr, "Warning: clock for SAA1099 specified, but not supported\n");

    // +CC: ES5503 clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x171 && clock != 0)
        fprintf(stderr, "Warning: clock for ES5503 specified, but not supported\n");

    // +D0: ES5505/ES5506 clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x171 && clock != 0)
        fprintf(stderr, "Warning: clock for ES5505/ES5506 specified, but not supported\n");

    // +D4: ES5503 output channels / ES5505/ES5506 amount of output channels / C352 clock divider
    if (offset + 4 > data_start)
        return data_start;
    dummy = parse_uint32(buffer, offset);

    // +D8: X1-010 clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x171 && clock != 0)
        fprintf(stderr, "Warning: clock for X1-010 specified, but not supported\n");

    // +DC: C352 clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x171 && clock != 0)
        fprintf(stderr, "Warning: clock for C352 specified, but not supported\n");

    // +E0: GA20 clock
    if (offset + 4 > data_start)
        return data_start;
    clock = parse_uint32(buffer, offset);
    if (version >= 0x171 && clock != 0)
        fprintf(stderr, "Warning: clock for GA20 specified, but not supported\n");

    return data_start;
}

void add_rom_data(chip_type type, ymfm::access_class access, std::vector<uint8_t> &buffer, uint32_t &localoffset, uint32_t size)
{
    uint32_t length = parse_uint32(buffer, localoffset);
    uint32_t start = parse_uint32(buffer, localoffset);
    for (int index = 0; index < 2; index++)
    {
        vgm_chip_base *chip = find_chip(type, index);
        if (chip != nullptr)
            chip->write_data(access, start, size, &buffer[localoffset]);
    }
}



float clamp_to_one(float in) {
    return fminf(fmaxf(in, -1.0f), 1.0f);
}

float int32_to_float(int32_t value) {
    /*
    float data = 0.0;
    if(value < 0) data = (float)(value / 32767.0);
    else data = (float)(value / 32768.0);
    return clamp_to_one(data);
    */
    //LOG_D("Input %d Output %f",value,clamp_to_one((float)(value / 32768.0)))
    return (float)(value /  32767.0);
}

inline int16_t int32_to_int16(int32_t value) {
    // preserve signedness
    return (int16_t)(value / 32767);
}

typedef struct {
    uint8_t streamId;
    uint8_t chipType; // YM2612 = 0x02
    uint8_t command;
    uint8_t reg;

    bool isRunning;

    uint8_t dataBank;

    uint8_t stepBase;
    uint8_t stepSize;

    uint32_t streamFrequency; // convert to timing map and use delta time in frame step?
    uint64_t streamPeriod;
    double frequencyInus; // (1/streamFrequency)*1000*1000


    uint32_t frameStep; // how many steps does it need to be written. (probably need more finer granularity?)

    uint8_t streamFlag; // TODO: support more mode

    bool isFastRun; // True if it should use blockId to look up
    uint16_t blockId; // block ID to look up in blockPtrs

    bool restart; // Restart stream with new settings

    // PRIVATE
    double _currentTime; // how many us had past
    bool _running; // is it actually in handling code?
    uint64_t _currentPosition;
} dacStream_t;

#include <list>

typedef struct {
    uint32_t location;
    uint32_t size;
    std::list<uint8_t> fifo;
} blockData_t;

int delay = 0;
static pthread_mutex_t dacStreamLock;
static dacStream_t dacStreams[2] = {0}; // TODO: Optimization. I do not think if there's stream that spans more than 32.
static blockData_t blockPtrs[0xFF] = {0};
//auto pcmBuffer = std::vector<uint8_t>();
uint8_t blockCounters = 0;
uint64_t nextBlockLoc = 0;

/*
void handlePCMWrite() {
    for(int i=0; i<4; i++) {
        dacStream_t* dac = &dacStreams[i];

        if(dac->isRunning) {

        }

    }
}*/

void generate_tick(std::vector<uint8_t> &buffer, uint32_t data_start, uint32_t output_rate, float *fill, size_t fill_size) {
    static uint32_t offset = data_start;
    static bool done = false;
    const emulated_time output_step = 0x100000000ull / output_rate;
    static emulated_time output_pos = 0;

    for(int i=0; i<fill_size; i+=2) {
        //handlePCMWrite();
        //handlePCMWrite();
        //handlePCMWrite();
        //handlePCMWrite();

        while (delay <= 0) {
            if (done) {
                offset = loop_offset;
                done = false;
            }
            if (offset + 1 >= buffer.size()) return;
            uint8_t cmd = buffer[offset++];
            //LOG_D("Next Cmd %02X",cmd);
            switch (cmd) {
                // YM2413, write value dd to register aa
                case 0x51:
                case 0xa1:
                    write_chip(CHIP_YM2413, cmd >> 7, buffer[offset], buffer[offset + 1]);
                    offset += 2;
                    break;

                    // YM2612 port 0, write value dd to register aa
                case 0x52:
                case 0xa2:
                    write_chip(CHIP_YM2612, cmd >> 7, buffer[offset], buffer[offset + 1]);
                    offset += 2;
                    break;

                    // YM2612 port 1, write value dd to register aa
                case 0x53:
                case 0xa3:
                    write_chip(CHIP_YM2612, cmd >> 7, buffer[offset] | 0x100,
                               buffer[offset + 1]);
                    offset += 2;
                    break;

                    // YM2151, write value dd to register aa
                case 0x54:
                case 0xa4:
                    write_chip(CHIP_YM2151, cmd >> 7, buffer[offset], buffer[offset + 1]);
                    offset += 2;
                    break;

                    // YM2203, write value dd to register aa
                case 0x55:
                case 0xa5:
                    write_chip(CHIP_YM2203, cmd >> 7, buffer[offset], buffer[offset + 1]);
                    offset += 2;
                    break;

                    // YM2608 port 0, write value dd to register aa
                case 0x56:
                case 0xa6:
                    write_chip(CHIP_YM2608, cmd >> 7, buffer[offset], buffer[offset + 1]);
                    offset += 2;
                    break;

                    // YM2608 port 1, write value dd to register aa
                case 0x57:
                case 0xa7:
                    write_chip(CHIP_YM2608, cmd >> 7, buffer[offset] | 0x100,
                               buffer[offset + 1]);
                    offset += 2;
                    break;

                    // YM2610 port 0, write value dd to register aa
                case 0x58:
                case 0xa8:
                    write_chip(CHIP_YM2610, cmd >> 7, buffer[offset], buffer[offset + 1]);
                    offset += 2;
                    break;

                    // YM2610 port 1, write value dd to register aa
                case 0x59:
                case 0xa9:
                    write_chip(CHIP_YM2610, cmd >> 7, buffer[offset] | 0x100,
                               buffer[offset + 1]);
                    offset += 2;
                    break;

                    // YM3812, write value dd to register aa
                case 0x5a:
                case 0xaa:
                    write_chip(CHIP_YM3812, cmd >> 7, buffer[offset], buffer[offset + 1]);
                    offset += 2;
                    break;

                    // YM3526, write value dd to register aa
                case 0x5b:
                case 0xab:
                    write_chip(CHIP_YM3526, cmd >> 7, buffer[offset], buffer[offset + 1]);
                    offset += 2;
                    break;

                    // Y8950, write value dd to register aa
                case 0x5c:
                case 0xac:
                    write_chip(CHIP_Y8950, cmd >> 7, buffer[offset], buffer[offset + 1]);
                    offset += 2;
                    break;

                    // YMF262 port 0, write value dd to register aa
                case 0x5e:
                case 0xae:
                    write_chip(CHIP_YMF262, cmd >> 7, buffer[offset], buffer[offset + 1]);
                    offset += 2;
                    break;

                    // YMF262 port 1, write value dd to register aa
                case 0x5f:
                case 0xaf:
                    write_chip(CHIP_YMF262, cmd >> 7, buffer[offset] | 0x100,
                               buffer[offset + 1]);
                    offset += 2;
                    break;

                    // Wait n samples, n can range from 0 to 65535 (approx 1.49 seconds)
                case 0x61:
                    delay = buffer[offset] | (buffer[offset + 1] << 8);
                    offset += 2;
                    break;

                    // wait 735 samples (60th of a second)
                case 0x62:
                    delay = 735;
                    break;

                    // wait 882 samples (50th of a second)
                case 0x63:
                    delay = 882;
                    break;

                    // end of sound data
                case 0x66:
                    done = true;
                    break;

                    // data block
                case 0x67: {
                    uint8_t dummy = buffer[offset++];
                    if (dummy != 0x66)
                        break;
                    uint8_t type = buffer[offset++];
                    uint32_t size = parse_uint32(buffer, offset);
                    uint32_t localoffset = offset;

                    switch (type) {
                        case 0x01: // RF5C68 PCM data for use with associated commands
                        case 0x02: // RF5C164 PCM data for use with associated commands
                        case 0x03: // PWM PCM data for use with associated commands
                        case 0x04: // OKIM6258 ADPCM data for use with associated commands
                        case 0x05: // HuC6280 PCM data for use with associated commands
                        case 0x06: // SCSP PCM data for use with associated commands
                        case 0x07: // NES APU DPCM data for use with associated commands
                            break;

                        case 0x00: // YM2612 PCM data for use with associated commands
                        {
                            pthread_mutex_lock(&dacStreamLock);
                            blockPtrs[blockCounters].size = size;
                            if(blockCounters == 0) {
                                blockPtrs[blockCounters].location = 0;
                            } else {
                                blockPtrs[blockCounters].location = nextBlockLoc;
                            }
                            nextBlockLoc += blockPtrs[blockCounters].size + 1;
                            //blockPtrs[blockCounters].fifo  new std::deque<uint8_t>();
                            //blockPtrs[blockCounters].fifo.resize(size);
                            std::copy_n(&buffer[localoffset],size,std::back_inserter(blockPtrs[blockCounters].fifo));
                            //blockPtrs[blockCounters].fifo.insert(blockPtrs[blockCounters].fifo.end(),buffer[localoffset],size);
                            pthread_mutex_unlock(&dacStreamLock);
                            LOG_I("Found YM2612 datablock; ptr is %08X; assigned to blockIdx %04X as loc %08X size %08X",localoffset,blockCounters,blockPtrs[blockCounters].location,blockPtrs[blockCounters].size);
                            // fixme: Very optimistic implementation; if VGM data doesn't have their data in linear, it won't work!
                            // fixme: Chip detection is bad
                            vgm_chip_base *chip = find_chip(CHIP_YM2612, 0);
                            if (chip != nullptr)
                                chip->write_data(ymfm::ACCESS_PCM, 0, size - 8,
                                                 &buffer[localoffset]);

                            blockCounters++;
                            break;
                        }

                        case 0x82: // YM2610 ADPCM ROM data
                            add_rom_data(CHIP_YM2610, ymfm::ACCESS_ADPCM_A, buffer, localoffset,
                                         size - 8);
                            break;

                        case 0x81: // YM2608 DELTA-T ROM data
                            add_rom_data(CHIP_YM2608, ymfm::ACCESS_ADPCM_B, buffer, localoffset,
                                         size - 8);
                            break;

                        case 0x83: // YM2610 DELTA-T ROM data
                            add_rom_data(CHIP_YM2610, ymfm::ACCESS_ADPCM_B, buffer, localoffset,
                                         size - 8);
                            break;

                        case 0x84: // YMF278B ROM data
                        case 0x87: // YMF278B RAM data
                            add_rom_data(CHIP_YMF278B, ymfm::ACCESS_PCM, buffer, localoffset,
                                         size - 8);
                            break;

                        case 0x88: // Y8950 DELTA-T ROM data
                            add_rom_data(CHIP_Y8950, ymfm::ACCESS_ADPCM_B, buffer, localoffset,
                                         size - 8);
                            break;

                        case 0x80: // Sega PCM ROM data
                        case 0x85: // YMF271 ROM data
                        case 0x86: // YMZ280B ROM data
                        case 0x89: // MultiPCM ROM data
                        case 0x8A: // uPD7759 ROM data
                        case 0x8B: // OKIM6295 ROM data
                        case 0x8C: // K054539 ROM data
                        case 0x8D: // C140 ROM data
                        case 0x8E: // K053260 ROM data
                        case 0x8F: // Q-Sound ROM data
                        case 0x90: // ES5505/ES5506 ROM data
                        case 0x91: // X1-010 ROM data
                        case 0x92: // C352 ROM data
                        case 0x93: // GA20 ROM data
                            break;

                        case 0xC0: // RF5C68 RAM write
                        case 0xC1: // RF5C164 RAM write
                        case 0xC2: // NES APU RAM write
                        case 0xE0: // SCSP RAM write
                        case 0xE1: // ES5503 RAM write
                            break;

                        default:
                            if (type >= 0x40 && type < 0x7f)
                                printf("Compressed data block not supported\n");
                            else
                                printf("Unknown data block type 0x%02X\n", type);
                            break;
                    }
                    offset += size;
                    break;
                }

                    // PCM RAM write
                case 0x68:
                    printf("68: PCM RAM write\n");
                    break;

                    // AY8910, write value dd to register aa
                case 0xa0:
                    write_chip(CHIP_YM2149, buffer[offset] >> 7, buffer[offset] & 0x7f,
                               buffer[offset + 1]);
                    offset += 2;
                    break;

                    // pp aa dd: YMF278B, port pp, write value dd to register aa
                case 0xd0:
                    write_chip(CHIP_YMF278B, buffer[offset] >> 7,
                               ((buffer[offset] & 0x7f) << 8) | buffer[offset + 1],
                               buffer[offset + 2]);
                    offset += 3;
                    break;

                case 0x70:
                case 0x71:
                case 0x72:
                case 0x73:
                case 0x74:
                case 0x75:
                case 0x76:
                case 0x77:
                case 0x78:
                case 0x79:
                case 0x7a:
                case 0x7b:
                case 0x7c:
                case 0x7d:
                case 0x7e:
                case 0x7f:
                    delay = (cmd & 15) + 1;
                    break;

                case 0x80:
                case 0x81:
                case 0x82:
                case 0x83:
                case 0x84:
                case 0x85:
                case 0x86:
                case 0x87:
                case 0x88:
                case 0x89:
                case 0x8a:
                case 0x8b:
                case 0x8c:
                case 0x8d:
                case 0x8e:
                case 0x8f: {
                    vgm_chip_base *chip = find_chip(CHIP_YM2612, 0);
                    if (chip != nullptr)
                        chip->write(0x2a, chip->read_pcm());
                    delay = cmd & 15;
                    break;
                }

                    // ignored, consume one byte
                case 0x30:
                case 0x31:
                case 0x32:
                case 0x33:
                case 0x34:
                case 0x35:
                case 0x36:
                case 0x37:
                case 0x38:
                case 0x39:
                case 0x3a:
                case 0x3b:
                case 0x3c:
                case 0x3d:
                case 0x3e:
                case 0x3f:
                case 0x4f:    // dd: Game Gear PSG stereo, write dd to port 0x06
                case 0x50:    // dd: PSG (SN76489/SN76496) write value dd
                    offset++;
                    break;

                    // ignored, consume two bytes
                case 0x40:
                case 0x41:
                case 0x42:
                case 0x43:
                case 0x44:
                case 0x45:
                case 0x46:
                case 0x47:
                case 0x48:
                case 0x49:
                case 0x4a:
                case 0x4b:
                case 0x4c:
                case 0x4d:
                case 0x4e:
                case 0x5d:    // aa dd: YMZ280B, write value dd to register aa
                case 0xb0:    // aa dd: RF5C68, write value dd to register aa
                case 0xb1:    // aa dd: RF5C164, write value dd to register aa
                case 0xb2:    // aa dd: PWM, write value ddd to register a (d is MSB, dd is LSB)
                case 0xb3:    // aa dd: GameBoy DMG, write value dd to register aa
                case 0xb4:    // aa dd: NES APU, write value dd to register aa
                case 0xb5:    // aa dd: MultiPCM, write value dd to register aa
                case 0xb6:    // aa dd: uPD7759, write value dd to register aa
                case 0xb7:    // aa dd: OKIM6258, write value dd to register aa
                case 0xb8:    // aa dd: OKIM6295, write value dd to register aa
                case 0xb9:    // aa dd: HuC6280, write value dd to register aa
                case 0xba:    // aa dd: K053260, write value dd to register aa
                case 0xbb:    // aa dd: Pokey, write value dd to register aa
                case 0xbc:    // aa dd: WonderSwan, write value dd to register aa
                case 0xbd:    // aa dd: SAA1099, write value dd to register aa
                case 0xbe:    // aa dd: ES5506, write value dd to register aa
                case 0xbf:    // aa dd: GA20, write value dd to register aa
                    offset += 2;
                    break;

                    // ignored, consume three bytes
                case 0xc9:
                case 0xca:
                case 0xcb:
                case 0xcc:
                case 0xcd:
                case 0xce:
                case 0xcf:
                case 0xd7:
                case 0xd8:
                case 0xd9:
                case 0xda:
                case 0xdb:
                case 0xdc:
                case 0xdd:
                case 0xde:
                case 0xdf:
                case 0xc0:    // bbaa dd: Sega PCM, write value dd to memory offset aabb
                case 0xc1:    // bbaa dd: RF5C68, write value dd to memory offset aabb
                case 0xc2:    // bbaa dd: RF5C164, write value dd to memory offset aabb
                case 0xc3:    // cc bbaa: MultiPCM, write set bank offset aabb to channel cc
                case 0xc4:    // mmll rr: QSound, write value mmll to register rr (mm - data MSB, ll - data LSB)
                case 0xc5:    // mmll dd: SCSP, write value dd to memory offset mmll (mm - offset MSB, ll - offset LSB)
                case 0xc6:    // mmll dd: WonderSwan, write value dd to memory offset mmll (mm - offset MSB, ll - offset LSB)
                case 0xc7:    // mmll dd: VSU, write value dd to memory offset mmll (mm - offset MSB, ll - offset LSB)
                case 0xc8:    // mmll dd: X1-010, write value dd to memory offset mmll (mm - offset MSB, ll - offset LSB)
                case 0xd1:    // pp aa dd: YMF271, port pp, write value dd to register aa
                case 0xd2:    // pp aa dd: SCC1, port pp, write value dd to register aa
                case 0xd3:    // pp aa dd: K054539, write value dd to register ppaa
                case 0xd4:    // pp aa dd: C140, write value dd to register ppaa
                case 0xd5:    // pp aa dd: ES5503, write value dd to register ppaa
                case 0xd6:    // pp aa dd: ES5506, write value aadd to register pp
                    offset += 3;
                    break;

                    // ignored, consume four bytes
                case 0xe0:    // dddddddd: Seek to offset dddddddd (Intel byte order) in PCM data bank of data block type 0 (YM2612).
                {
                    vgm_chip_base *chip = find_chip(CHIP_YM2612, 0);
                    uint32_t pos = parse_uint32(buffer, offset);
                    if (chip != nullptr)
                        chip->seek_pcm(pos);
                    offset += 4;
                    break;
                }
                case 0xe1:    // mmll aadd: C352, write value aadd to register mmll
                case 0xe2:
                case 0xe3:
                case 0xe4:
                case 0xe5:
                case 0xe6:
                case 0xe7:
                case 0xe8:
                case 0xe9:
                case 0xea:
                case 0xeb:
                case 0xec:
                case 0xed:
                case 0xee:
                case 0xef:
                case 0xf0:
                case 0xf1:
                case 0xf2:
                case 0xf3:
                case 0xf4:
                case 0xf5:
                case 0xf6:
                case 0xf7:
                case 0xf8:
                case 0xf9:
                case 0xfa:
                case 0xfb:
                case 0xfc:
                case 0xfd:
                case 0xfe:
                case 0xff:
                    offset += 4;
                    break;

                case 0x90: {
                    // Setup Stream
                    uint8_t streamId = buffer[offset];
                    uint8_t chipType = buffer[offset + 1]; // Find chip via chipType
                    uint8_t setCmd = buffer[offset + 2];
                    uint8_t reg = buffer[offset + 3];
                    LOG_I("Setup Stream %02X, ct %d writeStream at %02X:%02X", streamId, chipType,
                          setCmd, reg);
                    pthread_mutex_lock(&dacStreamLock);;
                    dacStreams[streamId].streamId = streamId;
                    dacStreams[streamId].chipType = chipType;
                    dacStreams[streamId].command = setCmd;
                    dacStreams[streamId].reg = reg;
                    pthread_mutex_unlock(&dacStreamLock);;
                    offset += 4;
                    break;
                }
                case 0x91: {
                    // Set Stream Data
                    uint8_t streamId = buffer[offset];
                    uint8_t dataBankId = buffer[offset + 1]; // Find chip via chipType; which datablock id should this stream look into?
                    uint8_t stepSize = buffer[offset + 2];
                    uint8_t stepBase = buffer[offset + 3];

                    pthread_mutex_lock(&dacStreamLock);;
                    dacStreams[streamId].stepBase = stepBase;
                    dacStreams[streamId].stepSize = stepSize;
                    pthread_mutex_unlock(&dacStreamLock);;

                    LOG_I("Set Stream %02X, dbId %d  stepSize %d stepBase %d", streamId, dataBankId,
                          stepSize, stepBase);
                    offset += 4;
                    break;
                }
                case 0x92: {
                    // Stream freq
                    uint8_t streamId = buffer[offset];
                    offset += 1;
                    uint32_t freq = parse_uint32(buffer, offset);

                    pthread_mutex_lock(&dacStreamLock);
                    dacStreams[streamId].frequencyInus = (1.0/freq) * 1000 * 1000;
                    //dacStreams[streamId].streamPeriod =
                    dacStreams[streamId].streamFrequency = freq;
                    pthread_mutex_unlock(&dacStreamLock);
                    LOG_I("cmd at f%08X streamId %02X Set Stream Freq at %08dHz; playing at %lf", offset, streamId, freq, dacStreams[streamId].frequencyInus);
                    //offset += 4;
                    break;
                }
                case 0x93: {
                    // Start Stream
                    uint8_t streamId = buffer[offset];
                    offset += 1;
                    uint32_t ds_offset = parse_uint32(buffer,offset);
                    //offset += 4;
                    uint8_t lengthMode = buffer[offset];
                    offset += 1;
                    uint32_t dataLength = parse_uint32(buffer,offset);
                    //offset += 4; // 10

                    // TODO: SUPPORT THIS COMMAND
                    pthread_mutex_lock(&dacStreamLock);;
                    pthread_mutex_unlock(&dacStreamLock);;
                    LOG_I("cmd at f%08X Start Stream %02X, startOfs %08X len %08X, with Mode %02X", offset, streamId,
                          ds_offset,dataLength,lengthMode);
                    break;
                }
                case 0x94:
                    // Stop stream 0xFF = stop all
                    pthread_mutex_lock(&dacStreamLock);;
                    dacStreams[buffer[offset]].isRunning = false;
                    pthread_mutex_unlock(&dacStreamLock);;
                    LOG_I("Stop Stream %02X", buffer[offset]);
                    offset++;
                    break;

                case 0x95: {
                    // fast call Start Stream
                    uint8_t streamId = buffer[offset];
                    uint16_t blockId = buffer[offset+2] << 8 | buffer[offset+1];
                    uint8_t flags = buffer[offset+3];

                    pthread_mutex_lock(&dacStreamLock);;
                    dacStreams[streamId].blockId = blockId;
                    dacStreams[streamId].streamFlag = flags;
                    dacStreams[streamId].isRunning = true;
                    dacStreams[streamId].restart = true;
                    pthread_mutex_unlock(&dacStreamLock);
                    LOG_I("Fast start Stream %02X, with blockId %04X (at %08X), flags %02X",
                          streamId,blockId,blockPtrs[blockId].location,flags);
                    offset += 4;
                    break;
                }

                default:
                    LOG_E("Unhandled command! 0x%02X at %08X", cmd, offset-1);
            }
        }
        if(delay) {
            int32_t outputs[2] = {0};
            for (auto &chip: active_chips) {
                chip->generate(output_pos, output_step, outputs);
            }
            output_pos += output_step;
            fill[i] = int32_to_float(outputs[0] *1.5);
            fill[i + 1] = int32_to_float(outputs[1]*1.5);
            delay--;
        }
    }
}


typedef struct {
    bool running;
    double timer;
    double runningTime;
    uint16_t pcmPtr;

    // Actual ptrs
    uint64_t size;
    uint64_t ptr;

} inStream_t;

/*
void *timer_handler(void *arg) {
    // check through
    inStream_t threadStream[4];
    struct timespec interval{};
    interval.tv_sec = 0;
    interval.tv_nsec = 1000;

    while(true) {
        for (auto & dacStream : dacStreams) {
            pthread_mutex_lock(&dacStreamLock);
            dacStream_t *dtr = &dacStream;
            uint16_t id = dtr->streamId;
            if(dtr->isRunning && dtr->restart) {
                threadStream[id].running = true;
                threadStream[id].pcmPtr = dtr->blockId;
                dtr->restart = false;
                threadStream[id].timer = threadStream[id].runningTime = 8.0;
            }
            pthread_mutex_unlock(&dacStreamLock);


            if(threadStream[id].running) {
                if((threadStream[id].runningTime -= 1.0) <= 0.0) {
                    if (blockPtrs[id].fifo.empty()) {
                        threadStream[id].running = false;
                        LOG_D("Stream %d finished",id);
                        continue;
                    }
                    write_chip(CHIP_YM2612, 0, 0x2a, blockPtrs[threadStream[id].pcmPtr].fifo.front() * 1.2);
                    blockPtrs[threadStream[id].pcmPtr].fifo.pop_front();

                    threadStream[id].runningTime = threadStream[id].timer;
                }
            }


        }

        nanosleep(&interval, nullptr);
    }
}
*/

/*
void *timer_handler(void *arg) {
    // check through
    struct timespec interval{};
    interval.tv_sec = 0;
    interval.tv_nsec = 1000;

    while(true) {
        pthread_mutex_lock(&dacStreamLock);
        for (auto & dacStream : dacStreams) {
            dacStream_t *dtr = &dacStream;
            uint16_t sId = dtr->streamId;
            if (!dtr->_running || dtr->restart) {
                //LOG_I("Thread: Got streamId %02X running!",i);
                // if it's JUST triggered

                dtr->_running = true;
                dtr->_currentTime = dtr->frequencyInus;
                dtr->_currentPosition = blockPtrs[sId].location;
            }
            if (dtr->_running) {
                // handling current sample
                if((dtr->_currentTime -= 1.0) < 0) {

                    if(dtr->_currentPosition >= blockPtrs[dtr->streamId].location + blockPtrs[dtr->streamId].size) {
                        dtr->_running = false;
                        dtr->isRunning = false;
                        continue;
                    }
                    // process frame immediate and set time again;
                    //

                    // put sample
                    vgm_chip_base* base = find_chip(CHIP_YM2612,0);
                    //base->seek_pcm(dtr->_currentPosition++);
                    base->write(0x2a,blockPtrs[sId].fifo.front());
                    blockPtrs[sId].fifo.pop_front();
                    if(dtr->_currentTime <= 0.0) dtr->_currentTime = dtr->frequencyInus;
                    // TODO:: PROPERLY IMPLEMENT
                }
            }

        }
        pthread_mutex_unlock(&dacStreamLock);
        nanosleep(&interval, nullptr);
    }
    return nullptr;
}
*/

class MyCallback : public oboe::AudioStreamDataCallback {
public:
    oboe::DataCallbackResult
    onAudioReady(oboe::AudioStream *audioStream, void *audioData, int numFrames) override {

        auto *outData = static_cast<float *>(audioData);

        //LOG_D("numFrames %d bpf %d fpb %d bps %d",numFrames,audioStream->getBytesPerFrame(),audioStream->getFramesPerBurst(),audioStream->getBytesPerSample())
        //for(int i=0; i<numFrames; ++i)
        //generate_tick(buffer, data_start, 44100, (float *)(outData + i*2),numFrames);
        generate_tick(vgm_buffer, vgm_data_start, 44100, outData,numFrames*2);

        return oboe::DataCallbackResult::Continue;
    }
};

class ErrorCallback : public oboe::AudioStreamErrorCallback {
public:
    void onErrorAfterClose(AudioStream *stream, Result error) override {
        LOG_I("%s() - error = %s",__func__,oboe::convertToText(error));
    }
};

std::shared_ptr<oboe::AudioStream> mStream;
std::shared_ptr<MyCallback> mMyCallback;
std::shared_ptr<ErrorCallback> mErrorCallback;

extern "C"
JNIEXPORT void JNICALL
Java_team_digitalfairy_ymfm_1thing_YmfmInterface_startOboe(JNIEnv *env, jclass clazz) {
    LOG_I("In Start!");

    mMyCallback = std::make_shared<MyCallback>();
    mErrorCallback = std::make_shared<ErrorCallback>();

    pthread_mutex_init(&dacStreamLock, nullptr);

    //oboe::DefaultStreamValues::FramesPerBurst = 1056;

    oboe::AudioStreamBuilder b;

    b.setDirection(oboe::Direction::Output);
    b.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    b.setSharingMode(oboe::SharingMode::Exclusive);
    //b.setFramesPerDataCallback(4096);
    b.setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Best);
    //b.setBufferCapacityInFrames(2400);
    b.setFormat(oboe::AudioFormat::Float);
    b.setSampleRate(44100);
    //b.setAudioApi(oboe::AudioApi::OpenSLES);

    //b.setChannelCount(oboe::ChannelCount::Mono);

    b.setDataCallback(mMyCallback);
    b.setErrorCallback(mErrorCallback);

    oboe::Result r = b.openStream(mStream);

    LOG_I("Ready")
    LOG_I("API = %s",mStream->getAudioApi() == oboe::AudioApi::AAudio?"AAudio":"OpenSLES");
    LOG_I("Format = %s",mStream->getFormat() == oboe::AudioFormat::I16?"I16":"Float");

    FILE *fp = fopen("/data/local/tmp/test.vgm","rb");
    if(fp == nullptr) {
        LOG_E("Error: File open failure %d %s",errno, strerror(errno));
        return;
    }

    fseek(fp, 0, SEEK_END);
    uint32_t sz = ftell(fp);
    rewind(fp);
    vgm_buffer = std::vector<uint8_t>(sz);

    auto bytes_read = fread(&vgm_buffer[0],1,sz,fp);
    // TODO: Error check
    // TODO: Decompress Gzip on memory

    fclose(fp);

    vgm_data_start = parse_header(vgm_buffer);

    // Make timer to scan through DACStream

    //pthread_t pthr;
    //int result = pthread_create(&pthr, nullptr, timer_handler, nullptr);
    /*
    struct sched_param param{};
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    pthread_setschedparam(pthr,SCHED_OTHER,&param);
    */
    /*
    timer_t timer;
    struct sigaction sa{};
    struct itimerspec itimer{};

    sa.sa_flags = SA_RESTART;
    sa.sa_handler = timer_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, nullptr);

    timer_create(CLOCK_REALTIME, nullptr,&timer);
    itimer.it_value.tv_sec = 0;
    itimer.it_value.tv_nsec = 5000; // 1us
    itimer.it_interval.tv_sec = 0;
    itimer.it_interval.tv_nsec = 5000;

    timer_settime(timer, 0, &itimer, nullptr);
    */
    // we are ready for actual output!
    r = mStream->requestStart();



}

// Step 1. load VGM file and prepare YMFM Context
// Step 2. Run down the VGM ticks bound to 44100; yet sound needs to be aligned to 44100
//

//     add_chips<ymfm::ym2151>(3579545, CHIP_YM2151, "YM2151");