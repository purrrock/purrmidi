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
	FILE: synth_organ.cpp
	VERSION: 0.1
	DATE: Feb 1st, 2019
	AUTHOR: Franco Caspe

Copyright [2019] [Franco Caspe]
SPDX-License-Identifier: Apache-2.0

*/

#include "synth_organ.hpp"
#include <algorithm>
#include <functional>

#ifdef TESTBENCH
#include <iostream>
#endif

#ifdef CHORUS_DEBUG
#include <iostream>
#endif

namespace synth
	{
	synth_organ::synth_organ()
		{
		init();
		}

	synth_organ::synth_organ(const synth_organ &other)
		{
		*this = other;
		}

	synth_organ &synth_organ::operator=(const synth_organ &other)
		{
		if (this != &other)
			{
			oscillators = other.oscillators;
			phase_counter = other.phase_counter;
			voice_stamp = other.voice_stamp;
			stamp_counter = other.stamp_counter;
			fx = other.fx;
			cc_drawbars = other.cc_drawbars;
			voice::voice_organ::init(static_cast<const cc::value_t*>(cc_drawbars.data()),
									&phase_counter);
			}
		return *this;
		}

	void synth_organ::init()
		{
		std::for_each(cc_drawbars.begin(),
					cc_drawbars.end(),
					[](cc::value_t &vector_value){vector_value = 127;});
		phase_counter = 0;
		voice_stamp.fill(0);
		stamp_counter = 0;
		reset_all();
		for (auto &osc : oscillators)
			{
			osc.set_attack(voice::clean_cut_value);
			osc.set_release(voice::clean_cut_value);
			}
		voice::voice_organ::init(static_cast<const cc::value_t*>(cc_drawbars.data()),
								&phase_counter);
		fx = efx::efx_chorus();
		#ifdef CHORUS_TEST
		fx.activate();
		#endif
		}

	void synth_organ::deactivate_all()
		{
		for (auto &osc : oscillators)
			{
			osc.deactivate();
			}
		}

	void synth_organ::reset_all()
		{
		for (auto &osc : oscillators)
			{
			osc.reset();
			}
		}

	void synth_organ::push_midi_cmd(const midi::command_t &cmd)
		{
		if(cmd.status == midi::status_t::NODATA)
			{
			#ifdef TESTBENCH
			//std::cout << "synth_organ::push_midi_cmd(): Got NODATA." << std::endl;
			#endif
			return;
			}
		if(cmd.status == midi::status_t::NOTE_ON)
			{
			if (cmd.data < tables::FEAT_LOWEST_GEAR || (cmd.data - tables::FEAT_LOWEST_GEAR) >= 61)
				{
				return;
				}
			const uint8_t osc_note = cmd.data - tables::FEAT_LOWEST_GEAR;

			// 1. Check if note is already active on a voice -> re-trigger/re-activate it
			auto existing = std::find_if(oscillators.begin(), oscillators.end(),
										 [osc_note](auto &osc) -> bool
										 { return osc.is_active() && osc.get_note() == osc_note; });
			if (existing != oscillators.end())
				{
				existing->activate(osc_note);
				voice_stamp[existing - oscillators.begin()] = ++stamp_counter;
				return;
				}

			// 2. Find an inactive voice
			auto inactive = std::find_if(oscillators.begin(), oscillators.end(),
										 [](auto &osc) -> bool
										 { return !osc.is_active(); });
			if (inactive != oscillators.end())
				{
				inactive->activate(osc_note);
				voice_stamp[inactive - oscillators.begin()] = ++stamp_counter;
				return;
				}

			// 3. Voice stealing if all voices are busy (see pick_voice_to_steal()).
			// activate() continues from the current envelope level, so no level drop/click.
			const size_t victim = pick_voice_to_steal();
			oscillators[victim].activate(osc_note);
			voice_stamp[victim] = ++stamp_counter;
			return;
			}
		if(cmd.status == midi::status_t::NOTE_OFF)
			{
			if (cmd.data < tables::FEAT_LOWEST_GEAR || (cmd.data - tables::FEAT_LOWEST_GEAR) >= 61)
				{
				return;
				}
			const uint8_t osc_note = cmd.data - tables::FEAT_LOWEST_GEAR;
			for (auto &osc : oscillators)
				{
				if (osc.is_active() && osc.get_note() == osc_note)
					{
					osc.deactivate();
					}
				}
			return;
			}
		if(cmd.status == midi::status_t::CONTROLLER_CHANGE)
			{

			if(cmd.data >= CC_ID_DRAWBAR_16 && cmd.data <= CC_ID_DRAWBAR_1)
				{
				const uint8_t drawbar_position = cmd.data - 20;
				cc_drawbars[drawbar_position] = cmd.value;
				}
			if(cmd.data == CC_ID_CHORUS_ON_OFF)
				{
				if(cmd.value <= 63)
					{
					fx.deactivate();
					}
				else
					{
					fx.activate();
					}
				}
			if(cmd.data == CC_ID_CHORUS_RATE)
				{
				fx.set_rate(cmd.value);
				}
			if(cmd.data == CC_ID_CHORUS_DEPTH)
				{
				fx.set_amplitude(cmd.value);
				}
			if(cmd.data == CC_ID_ENV_ATTACK)
				{
				std::for_each(oscillators.begin(),oscillators.end(),
				[&](auto &osc)
					{
					//Extend it to 16bit.
					const uint32_t env_value = static_cast<uint32_t>(cmd.value)<<8;
					osc.set_attack(env_value);
					});
				}
			if(cmd.data == CC_ID_ENV_RELEASE)
				{
				std::for_each(oscillators.begin(),oscillators.end(),
				[&](auto &osc)
					{
					//Extend it to 16bit.
					const uint32_t env_value = static_cast<uint32_t>(cmd.value)<<8;
					osc.set_release(env_value);
					});
				}
			return;
			}
		}
	size_t synth_organ::pick_voice_to_steal()
		{
		// 1. Quietest voice among those already in release.
		size_t best = FEAT_POLIPHONY;
		uint8_t best_level = 0xFF;
		for (size_t i = 0; i < FEAT_POLIPHONY; ++i)
			{
			if (oscillators[i].is_releasing())
				{
				const uint8_t level = oscillators[i].peek_level();
				if (best == FEAT_POLIPHONY || level < best_level)
					{
					best = i;
					best_level = level;
					}
				}
			}
		if (best != FEAT_POLIPHONY)
			{
			return best;
			}

		// 2. Oldest triggered voice (wrap-safe comparison of stamps).
		best = 0;
		for (size_t i = 1; i < FEAT_POLIPHONY; ++i)
			{
			if (static_cast<int32_t>(voice_stamp[i] - voice_stamp[best]) < 0)
				{
				best = i;
				}
			}
		return best;
		}

	void synth_organ::push_pb_cmd(const pushbutton::command_t &cmd)
		{
		(void)cmd;
		//Not implemented ...
		return;
		}

	synth::sample_t synth_organ::synthesize_sample()
		{
		synth::internal_sample_t current_sample = 0;
		for(auto &osc : oscillators)
			{
			if(osc.is_active())
				{
				current_sample += osc.get_sample();
				}
			}

		#ifdef TESTBENCH
		if(current_sample!=0)
		std::cout << "synth_organ::synthesize_sample(): Got sample: " << current_sample << std::endl;
		#endif
		update_phase_table();
		const efx::sample_t fx_output = fx.run_through(current_sample);
		#ifdef CHORUS_DEBUG
		std::cout << "synth_organ::synthesize_sample(): Got chorus sample: " << (int16_t)(fx_output>>24) << std::endl;
		#endif
		return to_external_sample(fx_output);
		}

	inline void synth_organ::update_phase_table()
		{
		++phase_counter;
		}

	inline synth::sample_t synth_organ::to_external_sample(synth::internal_sample_t sample)
			{
			int64_t s = sample >> OUTPUT_SHIFT;
			if (s > 32767) s = 32767;
			if (s < -32768) s = -32768;
			return static_cast<synth::sample_t>(s);
			}

	}