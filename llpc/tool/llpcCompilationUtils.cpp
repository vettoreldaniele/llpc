/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2021 Advanced Micro Devices, Inc. All Rights Reserved.
 *  Copyright (c) 2021 Google LLC. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  llpcCompilationUtils.cpp
 * @brief LLPC source file: contains the implementation of standalone LLPC compilation logic.
 ***********************************************************************************************************************
 */

#include "llpcCompilationUtils.h"
#include "llpcDebug.h"
#include "llpcInputUtils.h"
#include "llpcUtil.h"
#include "vkgcElfReader.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#ifndef LLPC_ENABLE_SPIRV_OPT
#define SPVGEN_STATIC_LIB 1
#endif

#include "spvgen.h"
#include "vfx.h"
#include <cassert>

using namespace llvm;
using namespace Vkgc;

namespace Llpc {
namespace StandaloneCompiler {

// =====================================================================================================================
// Callback function to allocate buffer for building shader module and building pipeline.
//
// @param instance : Placeholder instance object, unused
// @param userData : User data
// @param size : Requested allocation size
// @returns : Pointer to the allocated memory
void *allocateBuffer(void *instance, void *userData, size_t size) {
  (void)instance;
  void *allocBuf = malloc(size);
  memset(allocBuf, 0, size);

  assert(userData);
  auto *outBuf = reinterpret_cast<void **>(userData);
  *outBuf = allocBuf;
  return allocBuf;
}

// =====================================================================================================================
// Performs cleanup work for LLPC standalone compiler.
//
// @param [in/out] compileInfo : Compilation info of LLPC standalone tool
void cleanupCompileInfo(CompileInfo *compileInfo) {
  for (unsigned i = 0; i < compileInfo->shaderModuleDatas.size(); ++i) {
    // NOTE: We do not have to free SPIR-V binary for pipeline info file.
    // It will be freed when we close the VFX doc.
    if (!compileInfo->pipelineInfoFile)
      delete[] reinterpret_cast<const char *>(compileInfo->shaderModuleDatas[i].spirvBin.pCode);

    free(compileInfo->shaderModuleDatas[i].shaderBuf);
  }

  free(compileInfo->pipelineBuf);

  if (compileInfo->pipelineInfoFile)
    Vfx::vfxCloseDoc(compileInfo->pipelineInfoFile);
}

// =====================================================================================================================
// Translates GLSL source language to corresponding shader stage.
//
// @param sourceLang : GLSL source language
static ShaderStage sourceLangToShaderStage(SpvGenStage sourceLang) {
  static_assert(SpvGenStageVertex == 0, "Unexpected value!");
  static_assert(SpvGenStageTessControl == 1, "Unexpected value!");
  static_assert(SpvGenStageTessEvaluation == 2, "Unexpected value!");
  static_assert(SpvGenStageGeometry == 3, "Unexpected value!");
  static_assert(SpvGenStageFragment == 4, "Unexpected value!");
  static_assert(SpvGenStageCompute == 5, "Unexpected value!");

  return static_cast<ShaderStage>(sourceLang);
}

// =====================================================================================================================
// GLSL compiler, compiles GLSL source text file (input) to SPIR-V binary file (output).
//
// @param inFilename : Input filename, GLSL source text
// @param [out] stage : Shader stage
// @param [out] outFilename : Output filename, SPIR-V binary
// @param defaultEntryTarget : Default shader entry point name
// @returns : Result::Success on success, Result::ErrorUnavailable on failure.
Result compileGlsl(const std::string &inFilename, ShaderStage *stage, std::string &outFilename,
                   const std::string &defaultEntryTarget) {
  if (!InitSpvGen()) {
    LLPC_ERRS("Failed to load SPVGEN -- cannot compile GLSL\n");
    return Result::ErrorUnavailable;
  }

  Result result = Result::Success;
  bool isHlsl = false;

  SpvGenStage lang = spvGetStageTypeFromName(inFilename.c_str(), &isHlsl);
  if (lang == SpvGenStageInvalid) {
    LLPC_ERRS("File " << inFilename << ": Bad file extension; try -help\n");
    return Result::ErrorInvalidShader;
  }
  *stage = sourceLangToShaderStage(lang);

  FILE *inFile = fopen(inFilename.c_str(), "r");
  if (!inFile) {
    LLPC_ERRS("Fails to open input file: " << inFilename << "\n");
    result = Result::ErrorUnavailable;
  }

  FILE *outFile = nullptr;
  if (result == Result::Success) {
    outFilename = sys::path::filename(inFilename).str() + Ext::SpirvBin.str();

    outFile = fopen(outFilename.c_str(), "wb");
    if (!outFile) {
      LLPC_ERRS("Fails to open output file: " << outFilename << "\n");
      result = Result::ErrorUnavailable;
    }
  }

  if (result == Result::Success) {
    fseek(inFile, 0, SEEK_END);
    size_t textSize = ftell(inFile);
    fseek(inFile, 0, SEEK_SET);

    char *glslText = new char[textSize + 1];
    assert(glslText);
    memset(glslText, 0, textSize + 1);
    auto readSize = fread(glslText, 1, textSize, inFile);
    glslText[readSize] = 0;

    LLPC_OUTS("===============================================================================\n");
    LLPC_OUTS("// GLSL sources: " << inFilename << "\n\n");
    LLPC_OUTS(glslText);
    LLPC_OUTS("\n\n");

    int sourceStringCount = 1;
    const char *const *sourceList[1] = {};
    const char *fileName = inFilename.c_str();
    const char *const *fileList[1] = {&fileName};
    sourceList[0] = &glslText;

    void *program = nullptr;
    const char *log = nullptr;
    int compileOption = SpvGenOptionDefaultDesktop | SpvGenOptionVulkanRules | SpvGenOptionDebug;
    compileOption |= isHlsl ? SpvGenOptionReadHlsl : 0;
    const char *entryPoints[] = {defaultEntryTarget.c_str()};
    bool compileResult = spvCompileAndLinkProgramEx(1, &lang, &sourceStringCount, sourceList, fileList,
                                                    isHlsl ? entryPoints : nullptr, &program, &log, compileOption);

    LLPC_OUTS("// GLSL program compile/link log\n");

    if (compileResult) {
      const unsigned *spvBin = nullptr;
      unsigned binSize = spvGetSpirvBinaryFromProgram(program, 0, &spvBin);
      fwrite(spvBin, 1, binSize, outFile);

      textSize = binSize * 10 + 1024;
      char *spvText = new char[textSize];
      assert(spvText);
      memset(spvText, 0, textSize);
      LLPC_OUTS("\nSPIR-V disassembly: " << outFilename << "\n");
      spvDisassembleSpirv(binSize, spvBin, textSize, spvText);
      LLPC_OUTS(spvText << "\n");
      delete[] spvText;
    } else {
      LLPC_ERRS("Fail to compile GLSL sources\n\n" << log << "\n");
      result = Result::ErrorInvalidShader;
    }

    delete[] glslText;

    fclose(inFile);
    fclose(outFile);
  }

  return result;
}

// =====================================================================================================================
// SPIR-V assembler, converts SPIR-V assembly text file (input) to SPIR-V binary file (output).
//
// @param inFilename : Input filename, SPIR-V assembly text
// @param [out] outFilename : Output filename, SPIR-V binary
// @returns : Result::Success on success, Result::ErrorUnavailable on failure.
Result assembleSpirv(const std::string &inFilename, std::string &outFilename) {
  if (!InitSpvGen()) {
    LLPC_ERRS("Failed to load SPVGEN -- cannot assemble SPIR-V assembler source\n");
    return Result::ErrorUnavailable;
  }

  Result result = Result::Success;

  FILE *inFile = fopen(inFilename.c_str(), "r");
  if (!inFile) {
    LLPC_ERRS("Fails to open input file: " << inFilename << "\n");
    result = Result::ErrorUnavailable;
  }

  FILE *outFile = nullptr;
  if (result == Result::Success) {
    outFilename = sys::path::stem(sys::path::filename(inFilename)).str() + Ext::SpirvBin.str();

    outFile = fopen(outFilename.c_str(), "wb");
    if (!outFile) {
      LLPC_ERRS("Fails to open output file: " << outFilename << "\n");
      result = Result::ErrorUnavailable;
    }
  }

  if (result == Result::Success) {
    fseek(inFile, 0, SEEK_END);
    size_t textSize = ftell(inFile);
    fseek(inFile, 0, SEEK_SET);

    char *spvText = new char[textSize + 1];
    assert(spvText);
    memset(spvText, 0, textSize + 1);

    size_t realSize = fread(spvText, 1, textSize, inFile);
    spvText[realSize] = '\0';

    int binSize = realSize * 4 + 1024; // Estimated SPIR-V binary size
    unsigned *spvBin = new unsigned[binSize / sizeof(unsigned)];
    assert(spvBin);

    const char *log = nullptr;
    binSize = spvAssembleSpirv(spvText, binSize, spvBin, &log);
    if (binSize < 0) {
      LLPC_ERRS("Fails to assemble SPIR-V: \n" << log << "\n");
      result = Result::ErrorInvalidShader;
    } else {
      fwrite(spvBin, 1, binSize, outFile);

      LLPC_OUTS("===============================================================================\n");
      LLPC_OUTS("// SPIR-V disassembly: " << inFilename << "\n");
      LLPC_OUTS(spvText);
      LLPC_OUTS("\n\n");
    }

    fclose(inFile);
    fclose(outFile);

    delete[] spvText;
    delete[] spvBin;
  }

  return result;
}

// =====================================================================================================================
// Decodes the binary after building a pipeline and outputs the decoded info.
//
// @param pipelineBin : Pipeline binary
// @param [in/out] compileInfo : Compilation info of LLPC standalone tool
// @param isGraphics : Whether it is graphics pipeline
// @returns : Always returns Result::Success
Result decodePipelineBinary(const BinaryData *pipelineBin, CompileInfo *compileInfo, bool isGraphics) {
  // Ignore failure from ElfReader. It fails if pPipelineBin is not ELF, as happens with
  // -filetype=asm.
  ElfReader<Elf64> reader(compileInfo->gfxIp);
  size_t readSize = 0;
  if (reader.ReadFromBuffer(pipelineBin->pCode, &readSize) == Result::Success) {
    LLPC_OUTS("===============================================================================\n");
    LLPC_OUTS("// LLPC final ELF info\n");
    LLPC_OUTS(reader);
  }

  return Result::Success;
}

// =====================================================================================================================
// Builds shader module based on the specified SPIR-V binary.
//
// @param compiler : LLPC compiler object
// @param [in/out] compileInfo : Compilation info of LLPC standalone tool
// @returns : Result::Success on success, other return status on failure
Result buildShaderModules(ICompiler *compiler, CompileInfo *compileInfo) {
  Result result = Result::Success;

  for (unsigned i = 0; i < compileInfo->shaderModuleDatas.size(); ++i) {
    ShaderModuleBuildInfo *shaderInfo = &(compileInfo->shaderModuleDatas[i].shaderInfo);
    ShaderModuleBuildOut *shaderOut = &(compileInfo->shaderModuleDatas[i].shaderOut);

    shaderInfo->pInstance = nullptr; // Placeholder, unused.
    shaderInfo->pUserData = &(compileInfo->shaderModuleDatas[i].shaderBuf);
    shaderInfo->pfnOutputAlloc = allocateBuffer;
    shaderInfo->shaderBin = compileInfo->shaderModuleDatas[i].spirvBin;

    result = compiler->BuildShaderModule(shaderInfo, shaderOut);
    if (result != Result::Success && result != Result::Delayed) {
      LLPC_ERRS("Fails to build " << getShaderStageName(compileInfo->shaderModuleDatas[i].shaderStage)
                                  << " shader module:\n");
      break;
    }
  }

  return result;
}

// =====================================================================================================================
// Output LLPC resulting binary (ELF binary, ISA assembly text, or LLVM bitcode) to the specified target file.
//
// @param compileInfo : Compilation info of LLPC standalone tool
// @param suppliedOutFile : Name of the file to output ELF binary (specify "" to use base name of first input file with
// appropriate extension; specify "-" to use stdout)
// @param firstInFile : Name of first input file
// @returns : Result::Success on success, other return status on failure
Result outputElf(CompileInfo *compileInfo, const std::string &suppliedOutFile, StringRef firstInFile) {
  const BinaryData &pipelineBin = (compileInfo->stageMask & shaderStageToMask(ShaderStageCompute))
                                      ? compileInfo->compPipelineOut.pipelineBin
                                      : compileInfo->gfxPipelineOut.pipelineBin;
  SmallString<64> outFileName(suppliedOutFile);
  if (outFileName.empty()) {
    // Detect the data type as we are unable to access the values of the options "-filetype" and "-emit-llvm".
    StringLiteral ext = fileExtFromBinary(pipelineBin);
    outFileName = sys::path::filename(firstInFile);
    sys::path::replace_extension(outFileName, ext);
  }

  return writeFile(pipelineBin, outFileName);
}

} // namespace StandaloneCompiler
} // namespace Llpc