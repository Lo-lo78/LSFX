# LSFX patch for JUCE 8.0.12 only. Preserve the upstream license and all existing CC IDs.
if(NOT DEFINED JUCE_SOURCE_DIR)
    message(FATAL_ERROR "JUCE_SOURCE_DIR is required")
endif()
set(path "${JUCE_SOURCE_DIR}/modules/juce_audio_plugin_client/juce_audio_plugin_client_VST3.cpp")
file(READ "${path}" source)
string(FIND "${source}" "LSFX: append program mappings" already)
if(NOT already EQUAL -1)
    message(STATUS "LSFX JUCE multichannel program patch already applied")
    return()
endif()
function(lsfx_replace old new)
    string(FIND "${source}" "${old}" first)
    if(first EQUAL -1)
        message(FATAL_ERROR "Unexpected JUCE source: missing LSFX Program Change patch anchor: ${old}")
    endif()
    string(LENGTH "${old}" n)
    math(EXPR start "${first}+${n}")
    string(SUBSTRING "${source}" ${start} -1 rest)
    string(FIND "${rest}" "${old}" second)
    if(NOT second EQUAL -1)
        message(FATAL_ERROR "Ambiguous JUCE Program Change patch anchor: ${old}")
    endif()
    string(REPLACE "${old}" "${new}" result "${source}")
    set(source "${result}" PARENT_SCOPE)
endfunction()
lsfx_replace([==[MidiController parameterToMidiController[(int) numMIDIChannels * (int) Vst::kCountCtrlNumber];]==] [==[// LSFX: append program mappings; preserve all existing MIDI CC parameter IDs.
    static constexpr int lsfxLegacyMidiCount = (int) numMIDIChannels * (int) Vst::kCountCtrlNumber;
    static constexpr Vst::UnitID lsfxUnitBase = 0x4c530100;
    MidiController parameterToMidiController[lsfxLegacyMidiCount + numMIDIChannels];
    Vst::UnitID lsfxSelectedUnit = Vst::kRootUnitId;
    Vst::ParamID lsfxProgramID (int c) const noexcept
    {
        return parameterToMidiControllerOffset + static_cast<Vst::ParamID> (lsfxLegacyMidiCount + c);
    }
    int lsfxProgramChannel (Vst::ProgramListID list) const noexcept
    {
        const auto first = static_cast<Vst::ProgramListID> (lsfxProgramID (0));
        return list >= first && list < first + numMIDIChannels ? static_cast<int> (list - first) : -1;
    }]==])
lsfx_replace([==[        resultID = midiControllerToParameter[channel][midiControllerNumber];
        return kResultTrue; // Returning false makes some hosts stop asking for further MIDI Controller Assignments]==] [==[        if (busIndex != 0 || ! isPositiveAndBelow (static_cast<int> (channel), (int) numMIDIChannels))
            return kResultFalse;
        if (midiControllerNumber == Vst::kCtrlProgramChange)
        {
            resultID = lsfxProgramID (channel);
            return kResultTrue;
        }
        if (! isPositiveAndBelow (static_cast<int> (midiControllerNumber), (int) Vst::kCountCtrlNumber))
            return kResultFalse;
        resultID = midiControllerToParameter[channel][midiControllerNumber];
        return kResultTrue; // Returning false makes some hosts stop asking for further MIDI Controller Assignments]==])
lsfx_replace([==[                parameters.addParameter (new Vst::Parameter (toString ("MIDI CC " + String (c) + "|" + String (i)),
                                         static_cast<Vst::ParamID> (p) + parameterToMidiControllerOffset, nullptr, 0, 0,
                                         0, Vst::kRootUnitId));
            }
        }
    }]==] [==[                parameters.addParameter (new Vst::Parameter (toString ("MIDI CC " + String (c) + "|" + String (i)),
                                         static_cast<Vst::ParamID> (p) + parameterToMidiControllerOffset, nullptr, 0, 0,
                                         0, Vst::kRootUnitId));
            }
        }
        for (int c = 0; c < numMIDIChannels; ++c)
        {
            auto& mapping = parameterToMidiController[lsfxLegacyMidiCount + c];
            mapping.channel = c;
            mapping.ctrlNumber = Vst::kCtrlProgramChange;
            parameters.addParameter (new Vst::Parameter (toString ("MIDI Program Change CH " + String (c + 1)),
                                      lsfxProgramID (c), nullptr, 0, 127,
                                      Vst::ParameterInfo::kIsProgramChange | Vst::ParameterInfo::kCanAutomate,
                                      lsfxUnitBase + c));
        }
    }]==])
lsfx_replace([==[            else
                midiBuffer.addEvent (MidiMessage::controllerEvent (channel,
                                                                   jlimit (0, 127, ctrlNumber),]==] [==[            else if (ctrlNumber == Vst::kCtrlProgramChange)
                midiBuffer.addEvent (MidiMessage::programChange (channel,
                                                                 jlimit (0, 127, (int) (value * 128.0))), offsetSamples);
            else
                midiBuffer.addEvent (MidiMessage::controllerEvent (channel,
                                                                   jlimit (0, 127, ctrlNumber),]==])
lsfx_replace([==[        if (audioProcessor != nullptr)
            return audioProcessor->getUnitCount();]==] [==[        if (audioProcessor != nullptr)
            return audioProcessor->getUnitCount() + numMIDIChannels;]==])
lsfx_replace([==[        if (audioProcessor != nullptr)
            return audioProcessor->getUnitInfo (unitIndex, info);]==] [==[        if (audioProcessor != nullptr)
        {
            const auto base = audioProcessor->getUnitCount();
            if (unitIndex >= base && unitIndex < base + numMIDIChannels)
            {
                const auto c = unitIndex - base;
                info.id = lsfxUnitBase + c;
                info.parentUnitId = Vst::kRootUnitId;
                info.programListId = static_cast<Vst::ProgramListID> (lsfxProgramID (c));
                toString128 (info.name, "MIDI Channel " + String (c + 1));
                return kResultTrue;
            }
            const auto result = audioProcessor->getUnitInfo (unitIndex, info);
            if (result == kResultTrue && unitIndex == 0)
                info.programListId = Vst::kNoProgramListId; // LSFX has no global factory program switch.
            return result;
        }]==])
lsfx_replace([==[        if (audioProcessor != nullptr)
            return audioProcessor->getProgramListCount();]==] [==[        if (audioProcessor != nullptr)
            return numMIDIChannels;]==])
lsfx_replace([==[        if (audioProcessor != nullptr)
            return audioProcessor->getProgramListInfo (listIndex, info);]==] [==[        if (audioProcessor != nullptr && isPositiveAndBelow (listIndex, (int) numMIDIChannels))
        {
            info.id = static_cast<Vst::ProgramListID> (lsfxProgramID (listIndex));
            info.programCount = 128;
            toString128 (info.name, "MIDI Programs CH " + String (listIndex + 1));
            return kResultTrue;
        }
        if (audioProcessor != nullptr)
            return kResultFalse;]==])
lsfx_replace([==[        if (audioProcessor != nullptr)
            return audioProcessor->getProgramName (listId, programIndex, name);]==] [==[        if (audioProcessor != nullptr)
        {
            if (lsfxProgramChannel (listId) < 0 || ! isPositiveAndBelow (programIndex, 128))
                return kResultFalse;
            // Numeric names are bank-independent; actual patch names are in the LSFX editor.
            toString128 (name, "Program " + String (programIndex));
            return kResultTrue;
        }]==])
lsfx_replace([==[        if (audioProcessor != nullptr)
            return audioProcessor->getProgramInfo (listId, programIndex, attributeId, attributeValue);]==] [==[        if (audioProcessor != nullptr && lsfxProgramChannel (listId) >= 0)
            return kResultFalse;
        if (audioProcessor != nullptr)
            return audioProcessor->getProgramInfo (listId, programIndex, attributeId, attributeValue);]==])
lsfx_replace([==[        if (audioProcessor != nullptr)
            return audioProcessor->hasProgramPitchNames (listId, programIndex);]==] [==[        if (audioProcessor != nullptr && lsfxProgramChannel (listId) >= 0)
            return kResultFalse;
        if (audioProcessor != nullptr)
            return audioProcessor->hasProgramPitchNames (listId, programIndex);]==])
lsfx_replace([==[        if (audioProcessor != nullptr)
            return audioProcessor->getProgramPitchName (listId, programIndex, midiPitch, name);]==] [==[        if (audioProcessor != nullptr && lsfxProgramChannel (listId) >= 0)
            return kResultFalse;
        if (audioProcessor != nullptr)
            return audioProcessor->getProgramPitchName (listId, programIndex, midiPitch, name);]==])
lsfx_replace([==[        if (audioProcessor != nullptr)
            return audioProcessor->getUnitByBus (type, dir, busIndex, channel, unitId);]==] [==[        if (audioProcessor != nullptr && type == Vst::kEvent && dir == Vst::kInput && busIndex == 0
            && isPositiveAndBelow (channel, (int) numMIDIChannels))
        {
            unitId = lsfxUnitBase + channel;
            return kResultTrue;
        }
        if (audioProcessor != nullptr)
            return audioProcessor->getUnitByBus (type, dir, busIndex, channel, unitId);]==])
lsfx_replace([==[        if (audioProcessor != nullptr)
            return audioProcessor->selectUnit (unitId);]==] [==[        if (audioProcessor != nullptr && (unitId == Vst::kRootUnitId
            || (unitId >= lsfxUnitBase && unitId < lsfxUnitBase + numMIDIChannels)))
        {
            lsfxSelectedUnit = unitId;
            return kResultTrue;
        }
        if (audioProcessor != nullptr)
            return audioProcessor->selectUnit (unitId);]==])
lsfx_replace([==[        if (audioProcessor != nullptr)
            return audioProcessor->getSelectedUnit();]==] [==[        if (audioProcessor != nullptr)
            return lsfxSelectedUnit;]==])
file(WRITE "${path}" "${source}")
message(STATUS "LSFX: patched JUCE 8.0.12 for 16-channel MIDI Program Change")
