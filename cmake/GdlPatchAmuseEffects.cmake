# Native N64 REV_LEVEL (0x34), verified against the Factor 5 MusyX N64
# musician's reference and Legends' e008bfdc handler. Unknown commands must
# fail at decoding, not become null entries later dereferenced by the audio VM.
# e0092a04 reads voice+0x99/+0x9a: scale/128, then add, saturated to127.
# Default scale128/add0 leaves existing playback unchanged.
function(gdl_patch_amuse_effects source)
    gdl_patch_amuse_file("${source}" "include/amuse/AudioGroupPool.hpp"
        b215d21ffe25013c09f3da3ab0325eedd8798146f26da9579324b3a77574ce96
        306b2fe8512ff76cffa01c563ca5f556a459728a6964abfb070515235b1fb76a
        [=[#include <memory>]=]
        [=[#include <memory>
#include <stdexcept>]=]
        [=[    PitchWheelR,]=]
        [=[    PitchWheelR,
    ReverbLevel,]=]
        [=[  struct CmdSetPriority : ICmd {]=]
        [=[  struct CmdReverbLevel : ICmd {
    AT_DECL_DNA_YAMLV
    static const CmdIntrospection Introspective;
    Value<uint8_t> scale;
    Value<uint8_t> add;
    bool Do(SoundMacroState& st, Voice& vox) const override;
    CmdOp Isa() const override { return CmdOp::ReverbLevel; }
  };
  struct CmdSetPriority : ICmd {]=]
        [=[  const ICmd& getCmd(int i) const { return *m_cmds[assertPC(i)]; }]=]
        [=[  const ICmd& getCmd(int i) const {
    if (i < 0 || size_t(i) >= m_cmds.size() || !m_cmds[i])
      throw std::runtime_error("Unsupported or invalid MusyX macro command at step " + std::to_string(i));
    return *m_cmds[i];
  }]=]
    )
    gdl_patch_amuse_file("${source}" "lib/AudioGroupPool.cpp"
        ba092765e53010dacd84abd34b66caaf6a764375d6c329c0b0c54716d1f357ab
        1c3aab0e59c00ff91659b98c2dcb62afd2f6fe3ebf7cbce8152ca5b0f174afb4
        [=[    fmt::print(stderr, FMT_STRING("SoundMacro PC bounds exceeded [{}/{}]\n"), pc, int(m_cmds.size()));
    abort();]=]
        [=[    throw std::runtime_error("MusyX macro step exceeds command count: " + std::to_string(pc));]=]
        [=[void SoundMacro::readCmds(athena::io::IStreamReader& r, uint32_t size) {]=]
        [=[void SoundMacro::readCmds(athena::io::IStreamReader& r, uint32_t size) {
  if (size % 8 != 0)
    throw std::runtime_error("MusyX macro has a truncated command");]=]
        [=[    m_cmds.push_back(CmdDo<MakeCmdOp, std::unique_ptr<ICmd>>(mr));]=]
        [=[    auto cmd = CmdDo<MakeCmdOp, std::unique_ptr<ICmd>>(mr);
    if (!cmd)
      throw std::runtime_error("Unsupported MusyX macro opcode " + std::to_string(data[0] & 0xff) +
                               " at step " + std::to_string(i));
    m_cmds.push_back(std::move(cmd));]=]
        [=[  case CmdOp::PitchWheelR:
    return Op::template Do<CmdPitchWheelR>(std::forward<_Args>(args)...);]=]
        [=[  case CmdOp::PitchWheelR:
    return Op::template Do<CmdPitchWheelR>(std::forward<_Args>(args)...);
  case CmdOp::ReverbLevel:
    return Op::template Do<CmdReverbLevel>(std::forward<_Args>(args)...);]=]
        [=[  case CmdOp::PitchWheelR:
    return "PitchWheelR"sv;]=]
        [=[  case CmdOp::PitchWheelR:
    return "PitchWheelR"sv;
  case CmdOp::ReverbLevel:
    return "ReverbLevel"sv;]=]
        [=[  else if (!CompareCaseInsensitive(op.data(), "PitchWheelR"))
    return CmdOp::PitchWheelR;]=]
        [=[  else if (!CompareCaseInsensitive(op.data(), "PitchWheelR"))
    return CmdOp::PitchWheelR;
  else if (!CompareCaseInsensitive(op.data(), "ReverbLevel"))
    return CmdOp::ReverbLevel;]=]
    )
    gdl_patch_amuse_file("${source}" "include/amuse/SoundMacroState.hpp"
        07a89756d34aa2a8fe212f8c3bbf421b9f18f1dc727a8284971ce767795320ee
        fee10f8ef22fb1a2a09ac200b93673f9d6ffd54f026d6d0dfb9c2d936c1e3157
        [=[#include <cstdint>]=]
        [=[#include <algorithm>
#include <cstdint>]=]
        [=[  Evaluator m_reverbSel;]=]
        [=[  Evaluator m_reverbSel;
  uint8_t m_reverbScale = 128;
  uint8_t m_reverbAdd = 0;
  float applyReverbLevel(float volume) const {
    return std::clamp(std::clamp(volume, 0.f, 1.f) * (m_reverbScale / 128.f) +
                          m_reverbAdd / 127.f, 0.f, 1.f);
  }]=]
    )
    gdl_patch_amuse_file("${source}" "lib/SoundMacroState.cpp"
        a0c6112b9f1e6397478f2e891a2fc92cf6bfbef5d529857f014deb572d932c3f
        5400f3a155df2392b6664a5baf30780b519d0c550aca4e98c12852ca53104e34
        [=[  m_portamentoTime = 0.5f;]=]
        [=[  m_portamentoTime = 0.5f;
  m_reverbScale = 128;
  m_reverbAdd = 0;]=]
        [=[const SoundMacro::CmdIntrospection SoundMacro::CmdSetPriority::Introspective = {]=]
        [=[const SoundMacro::CmdIntrospection SoundMacro::CmdReverbLevel::Introspective = {
    CmdType::Setup,
    "Reverb Level"sv,
    "Scales channel reverb and adds a fixed reverb level."sv,
    {{{FIELD_HEAD(SoundMacro::CmdReverbLevel, scale), "Scale"sv, 0, 255, 128},
     {FIELD_HEAD(SoundMacro::CmdReverbLevel, add), "Add"sv, 0, 255, 0}}}};
bool SoundMacro::CmdReverbLevel::Do(SoundMacroState& st, Voice& vox) const {
  st.m_reverbScale = scale;
  st.m_reverbAdd = add;
  return false;
}

const SoundMacro::CmdIntrospection SoundMacro::CmdSetPriority::Introspective = {]=]
    )
    gdl_patch_amuse_file("${source}" "lib/atdna_AudioGroupPool.cpp"
        a8bb43138df1703b8fa967e32dc6174f0d2ef12936bf6cea7760a25c4d984cfc
        d09890309f5b00a8bc86e7dd755bb08ef30ab3c9f819dd5cf2ad33e84838a07e
        [=[// SetPriority: prio(1)]=]
        [=[// N64 REV_LEVEL: scale(1) add(1)
template <> void SoundMacro::CmdReverbLevel::Enumerate<DNAOpRead>(IStreamReader& r) {
    scale = r.readUByte(); add = r.readUByte();
}
template <> void SoundMacro::CmdReverbLevel::Enumerate<DNAOpWrite>(IStreamWriter& w) {
    w.writeUByte(scale); w.writeUByte(add);
}
template <> void SoundMacro::CmdReverbLevel::Enumerate<DNAOpBinarySize>(size_t& s) { s += 2; }
template <> void SoundMacro::CmdReverbLevel::Enumerate<DNAOpReadYaml>(YAMLDocReader&) {}
template <> void SoundMacro::CmdReverbLevel::Enumerate<DNAOpWriteYaml>(YAMLDocWriter&) {}
IMPL_YAMLV(SoundMacro::CmdReverbLevel)
std::string_view SoundMacro::CmdReverbLevel::DNAType() { return "amuse::SoundMacro::CmdReverbLevel"; }

// SetPriority: prio(1)]=]
    )
endfunction()
