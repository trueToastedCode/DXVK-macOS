#include "dxbc_analysis.h"
#include "dxbc_compiler.h"
#include "dxbc_module.h"

namespace dxvk {
  
  DxbcModule::DxbcModule(DxbcReader& reader)
  : m_header(reader) {
    for (uint32_t i = 0; i < m_header.numChunks(); i++) {
      
      // The chunk tag is stored at the beginning of each chunk
      auto chunkReader = reader.clone(m_header.chunkOffset(i));
      auto tag         = chunkReader.readTag();
      
      // The chunk size follows right after the four-character
      // code. This does not include the eight bytes that are
      // consumed by the FourCC and chunk length entry.
      auto chunkLength = chunkReader.readu32();
      
      chunkReader = chunkReader.clone(8);
      chunkReader = chunkReader.resize(chunkLength);
      
      if ((tag == "SHDR") || (tag == "SHEX"))
        m_shexChunk = new DxbcShex(chunkReader);
      
      if ((tag == "ISGN") || (tag == "ISG1"))
        m_isgnChunk = new DxbcIsgn(chunkReader, tag);
      
      if ((tag == "OSGN") || (tag == "OSG5") || (tag == "OSG1"))
        m_osgnChunk = new DxbcIsgn(chunkReader, tag);
      
      if ((tag == "PCSG") || (tag == "PSG1"))
        m_psgnChunk = new DxbcIsgn(chunkReader, tag);
    }
  }
  
  
  DxbcModule::~DxbcModule() {
    
  }
  
  
  Rc<DxvkShader> DxbcModule::compile(
    const DxbcModuleInfo& moduleInfo,
    const std::string&    fileName) const {
    if (m_shexChunk == nullptr)
      throw DxvkError("DxbcModule::compile: No SHDR/SHEX chunk");
    
    DxbcAnalysisInfo analysisInfo;
    
    DxbcAnalyzer analyzer(moduleInfo,
      m_shexChunk->programInfo(),
      m_isgnChunk, m_osgnChunk,
      m_psgnChunk, analysisInfo);
    
    this->runAnalyzer(analyzer, m_shexChunk->slice());
    
    DxbcCompiler compiler(
      fileName, moduleInfo,
      m_shexChunk->programInfo(),
      m_isgnChunk, m_osgnChunk,
      m_psgnChunk, analysisInfo);
    
    this->runCompiler(compiler, m_shexChunk->slice());
    
    return compiler.finalize();
  }
  
  
  Rc<DxvkShader> DxbcModule::compilePassthroughShader(
    const DxbcModuleInfo& moduleInfo,
    const std::string&    fileName) const {
    if (m_shexChunk == nullptr)
      throw DxvkError("DxbcModule::compile: No SHDR/SHEX chunk");
    
    DxbcAnalysisInfo analysisInfo;

    DxbcCompiler compiler(
      fileName, moduleInfo,
      DxbcProgramType::GeometryShader,
      m_osgnChunk, m_osgnChunk,
      m_psgnChunk, analysisInfo);
    
    compiler.processXfbPassthrough();
    return compiler.finalize();
  }


  Rc<DxvkShader> DxbcModule::compileFusedGs(
    const DxbcModule&     vsModule,
    const DxbcModuleInfo& vsInfo,
    const DxbcModuleInfo& gsInfo,
    const std::string&    fileName) const {
    if (m_shexChunk == nullptr || vsModule.m_shexChunk == nullptr)
      throw DxvkError("DxbcModule::compileFusedGs: No SHDR/SHEX chunk");

    if (m_shexChunk->programInfo().type() != DxbcProgramType::GeometryShader
     || vsModule.m_shexChunk->programInfo().type() != DxbcProgramType::VertexShader)
      throw DxvkError("DxbcModule::compileFusedGs: Invalid shader types");

    if (gsInfo.xfb != nullptr || vsInfo.xfb != nullptr)
      throw DxvkError("DxbcModule::compileFusedGs: Stream output not supported");

    // Compile the vertex shader into the shared module first
    DxbcAnalysisInfo vsAnalysis;

    DxbcAnalyzer vsAnalyzer(vsInfo,
      vsModule.m_shexChunk->programInfo(),
      vsModule.m_isgnChunk, vsModule.m_osgnChunk,
      vsModule.m_psgnChunk, vsAnalysis);

    vsModule.runAnalyzer(vsAnalyzer, vsModule.m_shexChunk->slice());

    DxbcCompiler vsCompiler(
      fileName + "_vs", vsInfo,
      vsModule.m_shexChunk->programInfo(),
      vsModule.m_isgnChunk, vsModule.m_osgnChunk,
      vsModule.m_psgnChunk, vsAnalysis,
      DxbcFusedMode::VsPart, nullptr);

    vsModule.runCompiler(vsCompiler, vsModule.m_shexChunk->slice());

    // Now compile the geometry shader into the same module
    DxbcAnalysisInfo gsAnalysis;

    DxbcAnalyzer gsAnalyzer(gsInfo,
      m_shexChunk->programInfo(),
      m_isgnChunk, m_osgnChunk,
      m_psgnChunk, gsAnalysis);

    this->runAnalyzer(gsAnalyzer, m_shexChunk->slice());

    DxbcCompiler gsCompiler(
      fileName, gsInfo,
      m_shexChunk->programInfo(),
      m_isgnChunk, m_osgnChunk,
      m_psgnChunk, gsAnalysis,
      DxbcFusedMode::GsPart, &vsCompiler);

    gsCompiler.setFusedCutCount(this->countEffectiveCuts(m_shexChunk->slice()));

    this->runCompiler(gsCompiler, m_shexChunk->slice());

    return gsCompiler.finalize();
  }


  uint32_t DxbcModule::countEffectiveCuts(DxbcCodeSlice slice) const {
    // Cuts that are not followed by any further emit do not
    // start a new strip. In shaders with loops, assume that
    // every cut can be followed by an emit.
    DxbcDecodeContext decoder;
    uint32_t cuts = 0;
    uint32_t effective = 0;
    bool hasLoop = false;

    while (!slice.atEnd()) {
      decoder.decodeInstruction(slice);
      const DxbcOpcode op = decoder.getInstruction().op;

      if (op == DxbcOpcode::Loop)
        hasLoop = true;

      const bool isEmit = op == DxbcOpcode::Emit || op == DxbcOpcode::EmitStream
        || op == DxbcOpcode::EmitThenCut || op == DxbcOpcode::EmitThenCutStream;
      const bool isCut = op == DxbcOpcode::Cut || op == DxbcOpcode::CutStream
        || op == DxbcOpcode::EmitThenCut || op == DxbcOpcode::EmitThenCutStream;

      // A plain emit makes all previous cuts effective
      if (op == DxbcOpcode::Emit || op == DxbcOpcode::EmitStream)
        effective = cuts;
      else if (isEmit)
        effective = cuts;

      if (isCut)
        cuts += 1;
    }

    return hasLoop ? cuts : effective;
  }


  void DxbcModule::runAnalyzer(
          DxbcAnalyzer&       analyzer,
          DxbcCodeSlice       slice) const {
    DxbcDecodeContext decoder;
    
    while (!slice.atEnd()) {
      decoder.decodeInstruction(slice);
      
      analyzer.processInstruction(
        decoder.getInstruction());
    }
  }
  
  
  void DxbcModule::runCompiler(
          DxbcCompiler&       compiler,
          DxbcCodeSlice       slice) const {
    DxbcDecodeContext decoder;
    
    while (!slice.atEnd()) {
      decoder.decodeInstruction(slice);
      
      compiler.processInstruction(
        decoder.getInstruction());
    }
  }
  
}
