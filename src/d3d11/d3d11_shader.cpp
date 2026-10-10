#include "d3d11_device.h"
#include "d3d11_shader.h"

namespace dxvk {
  
  D3D11CommonShader:: D3D11CommonShader() { }
  D3D11CommonShader::~D3D11CommonShader() { }
  
  
  D3D11CommonShader::D3D11CommonShader(
          D3D11Device*    pDevice,
    const DxvkShaderKey*  pShaderKey,
    const DxbcModuleInfo* pDxbcModuleInfo,
    const void*           pShaderBytecode,
          size_t          BytecodeLength) {
    const std::string name = pShaderKey->toString();
    Logger::debug(str::format("Compiling shader ", name));
    
    DxbcReader reader(
      reinterpret_cast<const char*>(pShaderBytecode),
      BytecodeLength);
    
    DxbcModule module(reader);
    
    // If requested by the user, dump both the raw DXBC
    // shader and the compiled SPIR-V module to a file.
    const std::string dumpPath = env::getEnvVar("DXVK_SHADER_DUMP_PATH");
    
    if (dumpPath.size() != 0) {
      reader.store(std::ofstream(str::tows(str::format(dumpPath, "/", name, ".dxbc").c_str()).c_str(),
        std::ios_base::binary | std::ios_base::trunc));
    }
    
    // Decide whether we need to create a pass-through
    // geometry shader for vertex shader stream output
    bool passthroughShader = pDxbcModuleInfo->xfb != nullptr
      && (module.programInfo().type() == DxbcProgramType::VertexShader
       || module.programInfo().type() == DxbcProgramType::DomainShader);

    if (module.programInfo().shaderStage() != pShaderKey->type() && !passthroughShader)
      throw DxvkError("Mismatching shader type.");

    m_shader = passthroughShader
      ? module.compilePassthroughShader(*pDxbcModuleInfo, name)
      : module.compile                 (*pDxbcModuleInfo, name);
    m_shader->setShaderKey(*pShaderKey);

    // Keep the byte code around so that vertex shaders can be combined
    // with geometry shaders if the device cannot run geometry shaders.
    const DxbcProgramType programType = module.programInfo().type();

    if (!passthroughShader && (programType == DxbcProgramType::VertexShader
     || programType == DxbcProgramType::GeometryShader)) {
      Rc<DxbcShaderSource> source = new DxbcShaderSource(
        pShaderBytecode, BytecodeLength, *pDxbcModuleInfo);
      m_shader->setSource(source);

      if (programType == DxbcProgramType::GeometryShader && !source->hasXfb()) {
        m_shader->setGsFusion([source, name, dumpPath] (const Rc<DxvkShader>& vs) -> Rc<DxvkShader> {
          Rc<DxbcShaderSource> vsSource = static_cast<DxbcShaderSource*>(vs->source().ptr());

          if (vsSource == nullptr || vsSource->hasXfb()
           || vs->info().stage != VK_SHADER_STAGE_VERTEX_BIT)
            return nullptr;

          DxbcReader vsReader(vsSource->code().data(), vsSource->code().size());
          DxbcReader gsReader(source->code().data(), source->code().size());

          DxbcModule vsModule(vsReader);
          DxbcModule gsModule(gsReader);

          const std::string fusedName = str::format(
            "GSEMU_", vs->getShaderKey().toString(), "_", name);

          Rc<DxvkShader> fused = gsModule.compileFusedGs(vsModule,
            vsSource->moduleInfo(), source->moduleInfo(), fusedName);

          fused->setShaderKey(DxvkShaderKey(VK_SHADER_STAGE_VERTEX_BIT,
            Sha1Hash::compute(fusedName.data(), fusedName.size())));

          if (!dumpPath.empty()) {
            std::ofstream dumpStream(
              str::tows(str::format(dumpPath, "/", fusedName, ".spv").c_str()).c_str(),
              std::ios_base::binary | std::ios_base::trunc);
            fused->dump(dumpStream);
          }

          Logger::info(str::format("GS emulation: ", fusedName,
            " (", fused->info().gsEmuVertexCount, " vertices per point)"));
          return fused;
        });
      }
    }
    
    if (dumpPath.size() != 0) {
      std::ofstream dumpStream(
        str::tows(str::format(dumpPath, "/", name, ".spv").c_str()).c_str(),
        std::ios_base::binary | std::ios_base::trunc);
      
      m_shader->dump(dumpStream);
    }
    
    // Create shader constant buffer if necessary
    const DxvkShaderCreateInfo& shaderInfo = m_shader->info();

    if (shaderInfo.uniformSize) {
      DxvkBufferCreateInfo info;
      info.size   = shaderInfo.uniformSize;
      info.usage  = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
      info.stages = util::pipelineStages(shaderInfo.stage);
      info.access = VK_ACCESS_UNIFORM_READ_BIT;
      
      VkMemoryPropertyFlags memFlags
        = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
        | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      
      m_buffer = pDevice->GetDXVKDevice()->createBuffer(info, memFlags);
      std::memcpy(m_buffer->mapPtr(0), shaderInfo.uniformData, shaderInfo.uniformSize);
    }

    pDevice->GetDXVKDevice()->registerShader(m_shader);
  }

  
  D3D11ShaderModuleSet:: D3D11ShaderModuleSet() { }
  D3D11ShaderModuleSet::~D3D11ShaderModuleSet() { }
  
  
  HRESULT D3D11ShaderModuleSet::GetShaderModule(
          D3D11Device*        pDevice,
    const DxvkShaderKey*      pShaderKey,
    const DxbcModuleInfo*     pDxbcModuleInfo,
    const void*               pShaderBytecode,
          size_t              BytecodeLength,
          D3D11CommonShader*  pShader) {
    // Use the shader's unique key for the lookup
    { std::unique_lock<dxvk::mutex> lock(m_mutex);
      
      auto entry = m_modules.find(*pShaderKey);
      if (entry != m_modules.end()) {
        *pShader = entry->second;
        return S_OK;
      }
    }
    
    // This shader has not been compiled yet, so we have to create a
    // new module. This takes a while, so we won't lock the structure.
    D3D11CommonShader module;
    
    try {
      module = D3D11CommonShader(pDevice, pShaderKey,
        pDxbcModuleInfo, pShaderBytecode, BytecodeLength);
    } catch (const DxvkError& e) {
      Logger::err(e.message());
      return E_INVALIDARG;
    }
    
    // Insert the new module into the lookup table. If another thread
    // has compiled the same shader in the meantime, we should return
    // that object instead and discard the newly created module.
    { std::unique_lock<dxvk::mutex> lock(m_mutex);
      
      auto status = m_modules.insert({ *pShaderKey, module });
      if (!status.second) {
        *pShader = status.first->second;
        return S_OK;
      }
    }
    
    *pShader = std::move(module);
    return S_OK;
  }
  
}
