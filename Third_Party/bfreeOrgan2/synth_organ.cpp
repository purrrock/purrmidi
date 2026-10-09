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
			phase_table = other.phase_table;
			fx = other.fx;
			cc_drawbars = other.cc_drawbars;
			voice::voice_organ::init(static_cast<const cc::value_t*>(cc_drawbars.data()),
									static_cast<const uint32_t*>(phase_table.data()));
			}
		return *this;
		}

	void synth_organ::init()
		{
		std::for_each(cc_drawbars.begin(),
					cc_drawbars.end(),
					[](cc::value_t &vector_value){vector_value = 127;});
		std::for_each(phase_table.begin(),
					phase_table.end(),
					[](uint32_t &phase){phase = 0;});
		for (auto &osc : oscillators)
			{
			osc.deactivate();
			}
		voice::voice_organ::init(static_cast<const cc::value_t*>(cc_drawbars.data()),
								static_cast<const uint32_t*>(phase_table.data()));
		fx = efx::efx_chorus();
		#ifdef CHORUS_TEST
		fx.activate();
		#endif
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
			auto osc_iterator = std::find_if(oscillators.begin(),oscillators.end(),
											[](auto &osc) -> bool
											{return !osc.is_active();});
			if(osc_iterator != oscillators.end())
				{
				osc_iterator->activate(osc_note);
				}
			#ifdef TESTBENCH
			std::cout << "synth_organ::push_midi_cmd(): Activate oscillator. Note: " << cmd.data << std::endl;
			#endif
			return;
			}
		if(cmd.status == midi::status_t::NOTE_OFF)
			{
			if (cmd.data < tables::FEAT_LOWEST_GEAR || (cmd.data - tables::FEAT_LOWEST_GEAR) >= 61)
				{
				return;
				}
			const uint8_t osc_note = cmd.data - tables::FEAT_LOWEST_GEAR;
			auto osc_iterator = std::find_if(oscillators.begin(),oscillators.end(),
								[&](auto &osc) -> bool
								{return osc.get_note() == osc_note ? true : false;});

			if(osc_iterator != oscillators.end())
				{
				osc_iterator->deactivate();
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
		std::transform (phase_table.begin(),	//First element start
						phase_table.end(),		//First element end
						tables::step_table.begin(),		//Second element start
						phase_table.begin(),	//Result element start
						std::plus<uint32_t>());	//Binary Operator
		}

	inline synth::sample_t synth_organ::to_external_sample(synth::internal_sample_t sample)
			{
			int64_t s = sample >> 20;
			if (s > 32767) s = 32767;
			if (s < -32768) s = -32768;
			return static_cast<synth::sample_t>(s);
			}

	}