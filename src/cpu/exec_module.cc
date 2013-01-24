/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <xenia/cpu/exec_module.h>

#include <llvm/Linker.h>
#include <llvm/PassManager.h>
#include <llvm/Analysis/Verifier.h>
#include <llvm/Bitcode/ReaderWriter.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/system_error.h>
#include <llvm/Support/Threading.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>

#include <xenia/cpu/codegen/module_generator.h>
#include <xenia/cpu/sdb.h>

#include "cpu/cpu-private.h"
#include "cpu/xethunk/xethunk.h"


using namespace llvm;
using namespace xe;
using namespace xe::cpu;
using namespace xe::cpu::codegen;
using namespace xe::kernel;


ExecModule::ExecModule(
    xe_memory_ref memory, shared_ptr<ExportResolver> export_resolver,
    UserModule* user_module, shared_ptr<llvm::ExecutionEngine>& engine) {
  memory_ = xe_memory_retain(memory);
  export_resolver_ = export_resolver;
  module_ = user_module;
  engine_ = engine;
  sdb_ = shared_ptr<sdb::SymbolDatabase>(
      new sdb::SymbolDatabase(memory_, export_resolver_.get(), module_));

  context_ = shared_ptr<LLVMContext>(new LLVMContext());
}

ExecModule::~ExecModule() {
  if (gen_module_) {
    Uninit();
    engine_->removeModule(gen_module_.get());
  }

  xe_memory_release(memory_);
}

int ExecModule::Prepare() {
  int result_code = 1;
  std::string error_message;

  char file_name[2048];

  OwningPtr<MemoryBuffer> shared_module_buffer;
  auto_ptr<Module> shared_module;
  auto_ptr<raw_ostream> outs;

  PassManager pm;
  PassManagerBuilder pmb;

  // TODO(benvanik): embed the bc file into the emulator.
  const char *thunk_path = "src/cpu/xethunk/xethunk.bc";

  // Calculate a cache path based on the module, the CPU version, and other
  // bits.
  // TODO(benvanik): cache path calculation.
  //const char *cache_path = "build/generated.bc";

  // Check the cache to see if the bitcode exists.
  // If it does, load that module directly. In the future we could also cache
  // on linked binaries but that requires more safety around versioning.
  // TODO(benvanik): check cache for module bitcode and load.
  // if (path_exists(cache_key)) {
  //   exec_module = load_bitcode(cache_key);
  //   sdb = load_symbol_table(cache_key);
  // }

  // If not found in cache, generate a new module.
  if (!gen_module_.get()) {
    // Load shared bitcode files.
    // These contain globals and common thunk code that are used by the
    // generated code.
    XEEXPECTZERO(MemoryBuffer::getFile(thunk_path, shared_module_buffer));
    shared_module = auto_ptr<Module>(ParseBitcodeFile(
        &*shared_module_buffer, *context_, &error_message));
    XEEXPECTNOTNULL(shared_module.get());

    // Analyze the module and add its symbols to the symbol database.
    XEEXPECTZERO(sdb_->Analyze());

    // Dump the symbol database.
    if (FLAGS_dump_module_map) {
      xesnprintf(file_name, XECOUNT(file_name),
          "%s%s.map", FLAGS_dump_path.c_str(), module_->name());
      sdb_->Write(file_name);
    }

    // Initialize the module.
    gen_module_ = shared_ptr<Module>(
        new Module(module_->name(), *context_.get()));
    // TODO(benavnik): addModuleFlag?

    // Inject globals.
    // This should be done ASAP to ensure that JITed functions can use the
    // constant addresses.
    XEEXPECTZERO(InjectGlobals());

    // Link shared module into generated module.
    // This gives us a single module that we can optimize and prevents the need
    // for foreward declarations.
    Linker::LinkModules(gen_module_.get(), shared_module.get(), 0,
                        &error_message);

    // Build the module from the source code.
    codegen_ = auto_ptr<ModuleGenerator>(new ModuleGenerator(
        memory_, export_resolver_.get(), module_, sdb_.get(),
        context_.get(), gen_module_.get()));
    XEEXPECTZERO(codegen_->Generate());

    // Write to cache.
    // TODO(benvanik): cache stuff

    // Dump pre-optimized module to disk.
    if (FLAGS_dump_module_bitcode) {
      xesnprintf(file_name, XECOUNT(file_name),
          "%s%s-preopt.bc", FLAGS_dump_path.c_str(), module_->name());
      outs = auto_ptr<raw_ostream>(new raw_fd_ostream(
          file_name, error_message, raw_fd_ostream::F_Binary));
      XEEXPECTTRUE(error_message.empty());
      WriteBitcodeToFile(gen_module_.get(), *outs);
    }
  }

  // Link optimizations.
  XEEXPECTZERO(gen_module_->MaterializeAllPermanently(&error_message));

  // Reset target triple (ignore what's in xethunk).
  gen_module_->setTargetTriple(llvm::sys::getDefaultTargetTriple());

  // Run full module optimizations.
  pm.add(new DataLayout(gen_module_.get()));
  if (FLAGS_optimize_ir_modules) {
    pm.add(createVerifierPass());
    pmb.OptLevel      = 3;
    pmb.SizeLevel     = 0;
    pmb.Inliner       = createFunctionInliningPass();
    pmb.Vectorize     = true;
    pmb.LoopVectorize = true;
    pmb.populateModulePassManager(pm);
    pmb.populateLTOPassManager(pm, false, true);
  }
  pm.add(createVerifierPass());
  pm.run(*gen_module_);

  // Dump post-optimized module to disk.
  if (FLAGS_optimize_ir_modules && FLAGS_dump_module_bitcode) {
    xesnprintf(file_name, XECOUNT(file_name),
        "%s%s.bc", FLAGS_dump_path.c_str(), module_->name());
    outs = auto_ptr<raw_ostream>(new raw_fd_ostream(
        file_name, error_message, raw_fd_ostream::F_Binary));
    XEEXPECTTRUE(error_message.empty());
    WriteBitcodeToFile(gen_module_.get(), *outs);
  }

  // TODO(benvanik): experiment with LLD to see if we can write out a dll.

  // Initialize the module.
  XEEXPECTZERO(Init());

  // Force JIT of all functions.
  for (Module::iterator it = gen_module_->begin(); it != gen_module_->end();
       ++it) {
    Function* fn = it;
    if (!fn->isDeclaration()) {
      engine_->getPointerToFunction(fn);
    }
  }

  result_code = 0;
XECLEANUP:
  return result_code;
}

int ExecModule::InjectGlobals() {
  LLVMContext& context = *context_.get();
  const DataLayout* dl = engine_->getDataLayout();
  Type* int8PtrTy = PointerType::getUnqual(Type::getInt8Ty(context));
  Type* intPtrTy = dl->getIntPtrType(context);
  GlobalVariable* gv;

  // xe_memory_base
  // This is the base void* pointer to the memory space.
  gv = new GlobalVariable(
      *gen_module_,
      int8PtrTy,
      true,
      GlobalValue::ExternalLinkage,
      0,
      "xe_memory_base");
  // Align to 64b - this makes SSE faster.
  gv->setAlignment(64);
  gv->setInitializer(ConstantExpr::getIntToPtr(
      ConstantInt::get(intPtrTy, (uintptr_t)xe_memory_addr(memory_, 0)),
      int8PtrTy));

  return 0;
}

int ExecModule::Init() {
  // Run static initializers. I'm not sure we'll have any, but who knows.
  engine_->runStaticConstructorsDestructors(gen_module_.get(), false);

  // Grab the init function and call it.
  Function* xe_module_init = gen_module_->getFunction("xe_module_init");
  std::vector<GenericValue> args;
  GenericValue ret = engine_->runFunction(xe_module_init, args);

  return ret.IntVal.getSExtValue();
}

int ExecModule::Uninit() {
  // Grab function and call it.
  Function* xe_module_uninit = gen_module_->getFunction("xe_module_uninit");
  std::vector<GenericValue> args;
  engine_->runFunction(xe_module_uninit, args);

  // Run static destructors.
  engine_->runStaticConstructorsDestructors(gen_module_.get(), true);

  return 0;
}

void ExecModule::Dump() {
  sdb_->Dump();
  gen_module_->dump();
}
