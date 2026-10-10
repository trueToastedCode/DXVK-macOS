#pragma once

#include "../dxvk/dxvk_shader.h"

#include "dxbc_chunk_isgn.h"
#include "dxbc_chunk_shex.h"
#include "dxbc_header.h"
#include "dxbc_modinfo.h"
#include "dxbc_reader.h"

// References used for figuring out DXBC:
// - https://github.com/tgjones/slimshader-cpp
// - Wine

namespace dxvk {
  
  class DxbcAnalyzer;
  class DxbcCompiler;

  /**
   * \brief DXBC shader source
   *
   * Keeps the original byte code and module info of a shader
   * around so that it can be recompiled later, e.g. when it
   * needs to be combined with a geometry shader.
   */
  class DxbcShaderSource : public RcObject {

  public:

    DxbcShaderSource(
      const void*           pBytecode,
            size_t          length,
      const DxbcModuleInfo& moduleInfo)
    : m_code(reinterpret_cast<const char*>(pBytecode),
             reinterpret_cast<const char*>(pBytecode) + length),
      m_moduleInfo(moduleInfo) {
      // Pointers refer to temporary data, so they are not kept
      m_hasXfb = moduleInfo.xfb != nullptr;
      m_moduleInfo.tess = nullptr;
      m_moduleInfo.xfb  = nullptr;
    }

    bool hasXfb() const { return m_hasXfb; }
    const std::vector<char>& code() const { return m_code; }
    const DxbcModuleInfo& moduleInfo() const { return m_moduleInfo; }

  private:

    std::vector<char> m_code;
    DxbcModuleInfo    m_moduleInfo;
    bool              m_hasXfb = false;

  };
  
  /**
   * \brief DXBC shader module
   * 
   * Reads the DXBC byte code and extracts information
   * about the resource bindings and the instruction
   * stream. A module can then be compiled to SPIR-V.
   */
  class DxbcModule {
    
  public:
    
    DxbcModule(DxbcReader& reader);
    ~DxbcModule();
    
    /**
     * \brief Shader type
     * \returns Shader type
     */
    DxbcProgramInfo programInfo() const {
      return m_shexChunk->programInfo();
    }
    
    /**
     * \brief Input and output signature chunks
     * 
     * Parts of the D3D11 API need access to the
     * input or output signature of the shader.
     */
    Rc<DxbcIsgn> isgn() const { return m_isgnChunk; }
    Rc<DxbcIsgn> osgn() const { return m_osgnChunk; }
    
    /**
     * \brief Compiles DXBC shader to SPIR-V module
     * 
     * \param [in] moduleInfo DXBC module info
     * \param [in] fileName File name, will be added to
     *        the compiled SPIR-V for debugging purposes.
     * \returns The compiled shader object
     */
    Rc<DxvkShader> compile(
      const DxbcModuleInfo& moduleInfo,
      const std::string&    fileName) const;
    
    /**
     * \brief Compiles a pass-through geometry shader
     *
     * Applications can pass a vertex shader to create
     * a geometry shader with stream output. In this
     * case, we have to create a passthrough geometry
     * shader, which operates in point to point mode.
     * \param [in] moduleInfo DXBC module info
     * \param [in] fileName SPIR-V shader name
     */
    Rc<DxvkShader> compilePassthroughShader(
      const DxbcModuleInfo& moduleInfo,
      const std::string&    fileName) const;

    /**
     * \brief Compiles vertex shader and this geometry shader
     *
     * Creates a vertex shader that emulates the given point list
     * to triangle strip geometry shader on hardware that does not
     * support geometry shaders. The shader is invoked once per
     * vertex that the geometry shader may emit (vertex index) and
     * once per input point (instance index).
     * \param [in] vsModule Vertex shader that feeds the GS
     * \param [in] vsInfo DXBC module info of the vertex shader
     * \param [in] gsInfo DXBC module info of this shader
     * \param [in] fileName SPIR-V shader name
     */
    Rc<DxvkShader> compileFusedGs(
      const DxbcModule&     vsModule,
      const DxbcModuleInfo& vsInfo,
      const DxbcModuleInfo& gsInfo,
      const std::string&    fileName) const;
    
  private:
    
    DxbcHeader   m_header;
    
    Rc<DxbcIsgn> m_isgnChunk;
    Rc<DxbcIsgn> m_osgnChunk;
    Rc<DxbcIsgn> m_psgnChunk;
    Rc<DxbcShex> m_shexChunk;
    
    uint32_t countEffectiveCuts(
            DxbcCodeSlice       slice) const;

    void runAnalyzer(
            DxbcAnalyzer&       analyzer,
            DxbcCodeSlice       slice) const;
    
    void runCompiler(
            DxbcCompiler&       compiler,
            DxbcCodeSlice       slice) const;
    
  };
  
}
