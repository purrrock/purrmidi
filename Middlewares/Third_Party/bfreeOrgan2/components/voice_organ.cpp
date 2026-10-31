/*
****************************************************************
*     _      __                                           ____
*    | |__  / _|_ __ ___  ___  ___  _ __ __ _  __ _ _ __ |___ \
*    | '_ \| |_| '__/ _ \/ _ \/ _ \| '__/ _` |/ _` | '_ \  __) |
*    | |_) |  _| | |  __/  __/ (_) | | | (_| | (_| | | | |/ __/
*    |_.__/|_| |_|  \___|\___|\___/|_|  \__, |\__,_|_| |_|_____|
*                                       |___/
*	  A flexynh based synchronous drawbar organ synthesizer.
****************************************************************
	FILE: voice_organ.cpp
	VERSION: 0.1
	DATE: Feb 1st, 2019
	AUTHOR: Franco Caspe

Copyright [2019] [Franco Caspe]
SPDX-License-Identifier: Apache-2.0

*/
#include <cstdint>
#include "voice_organ.hpp"

#ifdef TESTBENCH
#include <iostream>
#endif
namespace synth
	{
	namespace voice
		{
		const cc::value_t *voice_organ::_drawbars;
		const uint32_t *voice_organ::_phase_counter;

		voice_organ::voice_organ()
			{
			for(int t=0;t<9;t++)
				{
				_steps[t] = 0;
				}
			envelope.set_speed(env::env_feature_t::ATTACK,clean_cut_value);
			envelope.set_speed(env::env_feature_t::RELEASE,clean_cut_value);

			#ifdef TESTBENCH
			std::cout << "voice_organ::voice_organ() INFO: Voice Created!" << std::endl;
			#endif

			}

		void voice_organ::init(const cc::value_t *drawbar_vector,const uint32_t *phase_counter)
			{
			_drawbars = drawbar_vector;
			_phase_counter = phase_counter;
			}

		inline void voice_organ::activate(uint8_t note)
			{
			if(is_active() == true)
				{
				//We play a new note if we are active.
				//return;
				}
			envelope.attack();
			_is_active = true;
			my_note = note;
			for(int t=0;t<9;t++)
				{
				_steps[t] = tables::step_table[tables::switchbox[note*9 + t]];
				}
			}

		inline void voice_organ::deactivate()
			{
			envelope.release();
			}

		void voice_organ::reset()
			{
			_is_active = false;
			my_note = INVALID_NOTE;
			envelope.reset();
			}

		inline bool voice_organ::is_active()
			{
			return _is_active;
			}

		bool voice_organ::is_releasing()
			{
			return _is_active && envelope.is_releasing();
			}

		uint8_t voice_organ::peek_level()
			{
			#ifdef CLICK_TEST
			return 0xFF;
			#else
			return envelope.peek_level();
			#endif
			}

		void voice_organ::set_attack(uint32_t value)
			{
			envelope.set_speed(env::env_feature_t::ATTACK,value);
			}
		void voice_organ::set_release(uint32_t value)
			{
			envelope.set_speed(env::env_feature_t::RELEASE,value);
			}

		inline sample_t voice_organ::get_sample()
			{
			if(_is_active == false)
				{
				return static_cast<voice::sample_t>(0ul);
				}

			// Nine tonewheels, written so that all loads of one stage are independent of each other:
			//  stage 1: steps and drawbars, stage 2: wave table lookups. The compiler can then issue them back to
			// back instead of waiting for every single load result (a load-to-use stall is ~20 cycles here).
			const uint32_t counter = *_phase_counter;
			const uint32_t *const st = _steps;
			const cc::value_t *const db = _drawbars;
			// A macro, not a lambda: at -Os the compiler does not inline lambdas, and 9 calls were slower.
			#define PURR_WAVE(t) static_cast<int32_t>(tables::wave_sine[tables::phase_to_wave_index(counter*st[t])])
			const int32_t w0 = PURR_WAVE(0), w1 = PURR_WAVE(1), w2 = PURR_WAVE(2), w3 = PURR_WAVE(3), w4 = PURR_WAVE(4);
			const int32_t w5 = PURR_WAVE(5), w6 = PURR_WAVE(6), w7 = PURR_WAVE(7), w8 = PURR_WAVE(8);
			#undef PURR_WAVE
			voice::sample_t sample = w0*static_cast<uint8_t>(db[0]) + w1*static_cast<uint8_t>(db[1])
								+ w2*static_cast<uint8_t>(db[2]) + w3*static_cast<uint8_t>(db[3])
								+ w4*static_cast<uint8_t>(db[4]) + w5*static_cast<uint8_t>(db[5])
								+ w6*static_cast<uint8_t>(db[6]) + w7*static_cast<uint8_t>(db[7])
								+ w8*static_cast<uint8_t>(db[8]);

			const uint8_t env_level = static_cast<uint8_t>(envelope.get_level());

			//Check activity of envelope generator.
			_is_active = envelope.is_running();

			// Multiply tonewheel sum by envelope level in int64_t to prevent signed 32-bit integer overflow,
			// then safely clamp to int32_t range before returning.
			int64_t full_sample = static_cast<int64_t>(sample) * static_cast<int64_t>(env_level);
			if (full_sample > 2147483647LL) {
				full_sample = 2147483647LL;
			} else if (full_sample < -2147483648LL) {
				full_sample = -2147483648LL;
			}

			#ifdef TESTBENCH
			if(sample!=0)
			std::cout << "voice_organ::get_sample() INFO: Got Sample: " << std::hex << sample << std::endl;
			#endif
			return static_cast<voice::sample_t>(full_sample);
			}

		void voice_organ::render_block(int64_t *acc,uint32_t n,uint32_t counter0)
			{
			while(n != 0 && _is_active)
				{
				uint8_t levels[64];
				const uint32_t chunk = (n > 64u) ? 64u : n;
				const uint32_t k = envelope.get_level_block(levels,chunk);
				_is_active = envelope.is_running();

				const uint32_t s0 = _steps[0], s1 = _steps[1], s2 = _steps[2], s3 = _steps[3], s4 = _steps[4];
				const uint32_t s5 = _steps[5], s6 = _steps[6], s7 = _steps[7], s8 = _steps[8];
				// Drawbars only change between blocks (control changes are applied before rendering).
				const int32_t d0 = static_cast<uint8_t>(_drawbars[0]), d1 = static_cast<uint8_t>(_drawbars[1]);
				const int32_t d2 = static_cast<uint8_t>(_drawbars[2]), d3 = static_cast<uint8_t>(_drawbars[3]);
				const int32_t d4 = static_cast<uint8_t>(_drawbars[4]), d5 = static_cast<uint8_t>(_drawbars[5]);
				const int32_t d6 = static_cast<uint8_t>(_drawbars[6]), d7 = static_cast<uint8_t>(_drawbars[7]);
				const int32_t d8 = static_cast<uint8_t>(_drawbars[8]);
				const int16_t *const wave = tables::wave_sine.data();

				for(uint32_t i = 0; i < k; ++i)
					{
					const uint32_t c = counter0 + i;
					#define PURR_W(s) static_cast<int32_t>(wave[tables::phase_to_wave_index(c*(s))])
					const voice::sample_t sample = PURR_W(s0)*d0 + PURR_W(s1)*d1 + PURR_W(s2)*d2
												+ PURR_W(s3)*d3 + PURR_W(s4)*d4 + PURR_W(s5)*d5
												+ PURR_W(s6)*d6 + PURR_W(s7)*d7 + PURR_W(s8)*d8;
					#undef PURR_W
					int64_t full_sample = static_cast<int64_t>(sample) * static_cast<int64_t>(levels[i]);
					if (full_sample > 2147483647LL) {
						full_sample = 2147483647LL;
					} else if (full_sample < -2147483648LL) {
						full_sample = -2147483648LL;
					}
					acc[i] += full_sample;
					}
				acc += k;
				counter0 += k;
				n -= k;
				}
			}

		void voice_organ::render_block_bent(int64_t *acc,uint32_t n,uint32_t counter0,uint32_t frac0,int32_t delta_q32)
			{
			// 64-bit time base: integer sample counter in the upper half, fraction of a sample in the lower half.
			uint64_t pos = (static_cast<uint64_t>(counter0) << 32) | frac0;
			const uint64_t step64 = (1ull << 32) + static_cast<uint64_t>(static_cast<int64_t>(delta_q32));
			while(n != 0 && _is_active)
				{
				uint8_t levels[64];
				const uint32_t chunk = (n > 64u) ? 64u : n;
				const uint32_t k = envelope.get_level_block(levels,chunk);
				_is_active = envelope.is_running();

				const uint32_t s0 = _steps[0], s1 = _steps[1], s2 = _steps[2], s3 = _steps[3], s4 = _steps[4];
				const uint32_t s5 = _steps[5], s6 = _steps[6], s7 = _steps[7], s8 = _steps[8];
				const int32_t d0 = static_cast<uint8_t>(_drawbars[0]), d1 = static_cast<uint8_t>(_drawbars[1]);
				const int32_t d2 = static_cast<uint8_t>(_drawbars[2]), d3 = static_cast<uint8_t>(_drawbars[3]);
				const int32_t d4 = static_cast<uint8_t>(_drawbars[4]), d5 = static_cast<uint8_t>(_drawbars[5]);
				const int32_t d6 = static_cast<uint8_t>(_drawbars[6]), d7 = static_cast<uint8_t>(_drawbars[7]);
				const int32_t d8 = static_cast<uint8_t>(_drawbars[8]);
				const int16_t *const wave = tables::wave_sine.data();

				for(uint32_t i = 0; i < k; ++i)
					{
					const uint32_t ci = static_cast<uint32_t>(pos >> 32);
					const uint64_t cf = static_cast<uint32_t>(pos);
					// phase = (ci + cf/2^32) * step  (mod 2^32)
					#define PURR_WB(s) static_cast<int32_t>(wave[tables::phase_to_wave_index(ci*(s) + static_cast<uint32_t>((cf*(s)) >> 32))])
					const voice::sample_t sample = PURR_WB(s0)*d0 + PURR_WB(s1)*d1 + PURR_WB(s2)*d2
												+ PURR_WB(s3)*d3 + PURR_WB(s4)*d4 + PURR_WB(s5)*d5
												+ PURR_WB(s6)*d6 + PURR_WB(s7)*d7 + PURR_WB(s8)*d8;
					#undef PURR_WB
					int64_t full_sample = static_cast<int64_t>(sample) * static_cast<int64_t>(levels[i]);
					if (full_sample > 2147483647LL) {
						full_sample = 2147483647LL;
					} else if (full_sample < -2147483648LL) {
						full_sample = -2147483648LL;
					}
					acc[i] += full_sample;
					pos += step64;
					}
				acc += k;
				n -= k;
				}
			}

		inline voice::sample_t voice_organ::get_tonewheel(uint8_t tone)
			{
			const uint8_t tonewheel_note = tables::switchbox[my_note*9 + tone];

			// Tonewheel phase = sample counter * step (mod 2^32). Identical to accumulating the step every
			// sample into a per-tonewheel phase table, but needs no 79-entry table update per sample.
			const uint32_t phase = (*_phase_counter) * tables::step_table[tonewheel_note];
			const uint16_t wave_index = tables::phase_to_wave_index(phase);

			const voice::sample_t wave_value = static_cast<voice::sample_t>(tables::wave_sine[wave_index]);

			const uint8_t drawbar_value = static_cast<uint8_t>(_drawbars[tone]);

			return static_cast<voice::sample_t>(wave_value*drawbar_value);
			}
		}
	}