//------------------------------------------------------------------------------
// Hila preprocessor for lattice simulation programs
// Converts special c++ dialect to hardware-dependent "kernels".
//
// Uses Clang RecursiveASTVisitor and Rewriter
// interfaces
//
// Kari Rummukainen
// Jarno Rantaharju
//
//------------------------------------------------------------------------------
#include <sstream>
#include <iostream>
#include <string>
// #include <filesystem>  <- this should be used for pathname resolution, but llvm-9 does
// not properly handle

#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/ASTConsumers.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Rewrite/Core/Rewriter.h"

#include "hilapp.h"
#include "optionsparser.h"
#include "stringops.h"
#include "toplevelvisitor.h"
#include "specialization_db.h"

// definitions for global variables
ClassTemplateDecl *field_decl = nullptr;
ClassTemplateDecl *field_storage_decl = nullptr;
const std::string field_storage_type = "field_storage<";
const std::string field_type = "Field<";
std::list<field_ref> field_ref_list = {};
std::list<field_info> field_info_list = {};
std::list<array_ref> array_ref_list = {};
std::list<vector_reduction_ref> vector_reduction_ref_list = {};
std::list<special_function_call> special_function_call_list = {};
std::vector<Expr *> remove_expr_list = {};

bool state::compile_errors_occurred = false;

bool skip_this_translation_unit = false;

/// definition of command line options
llvm::cl::OptionCategory HilappCategory(program_name);

llvm::cl::opt<bool> cmdline::dump_ast("dump-ast", llvm::cl::desc("Dump AST tree"),
                                      llvm::cl::cat(HilappCategory));

llvm::cl::opt<std::string>
    cmdline::dummy_def("D", llvm::cl::value_desc("macro[=value]"),
                       llvm::cl::desc("Define name/macro for preprocessor"),
                       llvm::cl::cat(HilappCategory));

llvm::cl::opt<std::string>
    cmdline::dummy_incl("I", llvm::cl::value_desc("directory"),
                        llvm::cl::desc("Directory for include file search"),
                        llvm::cl::cat(HilappCategory));

llvm::cl::opt<bool> cmdline::function_spec_no_inline(
    "function-spec-no-inline",
    llvm::cl::desc("Do not mark generated function specializations \"inline\""),
    llvm::cl::cat(HilappCategory));

llvm::cl::opt<bool> cmdline::method_spec_no_inline(
    "method-spec-no-inline",
    llvm::cl::desc("Do not mark generated method specializations \"inline\""),
    llvm::cl::cat(HilappCategory));

llvm::cl::opt<bool> cmdline::allow_func_globals(
    "allow-func-globals",
    llvm::cl::desc(
        "Allow using global or extern variables in functions called from site loops."
        "\nThis will not work in kernelized code (for example GPU)"),
    llvm::cl::cat(HilappCategory));

llvm::cl::opt<bool>
    cmdline::funcinfo("ident-functions",
                      llvm::cl::desc("Comment function call types in output"),
                      llvm::cl::cat(HilappCategory));

llvm::cl::opt<bool> cmdline::no_output("no-output",
                                       llvm::cl::desc("No output file, for syntax check"),
                                       llvm::cl::cat(HilappCategory));

llvm::cl::opt<bool> cmdline::syntax_only("syntax-only",
                                         llvm::cl::desc("Same as no-output"),
                                         llvm::cl::cat(HilappCategory));

llvm::cl::opt<std::string> cmdline::output_filename(
    "o", llvm::cl::desc("Output file (default: <file>.cpt, write to stdout: -o - "),
    llvm::cl::value_desc("filename"), llvm::cl::cat(HilappCategory), llvm::cl::Prefix);

llvm::cl::opt<bool>
    cmdline::no_mpi("no-mpi",
                    llvm::cl::desc("Do not generate MPI specific code (single node)"),
                    llvm::cl::cat(HilappCategory));

llvm::cl::opt<bool> cmdline::no_interleaved_comm(
    "no-interleave", llvm::cl::desc("Do not interleave communications with computation"),
    llvm::cl::cat(HilappCategory));

// List of targets that can be specified in command line arguments

llvm::cl::opt<bool>
    cmdline::kernel("target:vanilla-kernel",
                    llvm::cl::desc("Generate site loop 'kernels' for vanilla code"),
                    llvm::cl::cat(HilappCategory));

llvm::cl::opt<bool> cmdline::vanilla("target:vanilla",
                                     llvm::cl::desc("Generate loops in place"),
                                     llvm::cl::cat(HilappCategory));

llvm::cl::opt<bool> cmdline::CUDA("target:CUDA", llvm::cl::desc("Generate CUDA kernels"),
                                  llvm::cl::cat(HilappCategory));

llvm::cl::opt<bool> cmdline::AVX512("target:AVX512",
                                    llvm::cl::desc("Generate AVX512 vectorized loops"),
                                    llvm::cl::cat(HilappCategory));

llvm::cl::opt<bool> cmdline::AVX("target:AVX",
                                 llvm::cl::desc("Generate AVX vectorized loops"),
                                 llvm::cl::cat(HilappCategory));

llvm::cl::opt<bool> cmdline::SSE("target:SSE",
                                 llvm::cl::desc("Generate SSE vectorized loops"),
                                 llvm::cl::cat(HilappCategory));

llvm::cl::opt<int> cmdline::vectorize(
    "target:vectorize",
    llvm::cl::desc("Generate vectorized loops with given vector size \n"
                   "For example -target:vectorize=32 is equivalent to -target:AVX"),
    llvm::cl::cat(HilappCategory));

llvm::cl::opt<bool> cmdline::openacc("target:openacc",
                                     llvm::cl::desc("Offload to GPU using openACC"),
                                     llvm::cl::cat(HilappCategory));

// Debug and Utility arguments

// llvm::cl::opt<bool> cmdline::func_attribute("function-attributes",
//       llvm::cl::desc("write pragmas/attributes to functions called from loops"),
//       llvm::cl::cat(HilappCategory));

llvm::cl::opt<int>
    cmdline::verbosity("verbosity",
                       llvm::cl::desc("Verbosity level 0-2.  Default 0 (quiet)"),
                       llvm::cl::cat(HilappCategory));

llvm::cl::opt<int>
    cmdline::avx_info("AVXinfo",
                      llvm::cl::desc("AVX vectorization information level 0-2. 0 quiet, "
                                     "1 not vectorizable loops, 2 all loops"),
                      llvm::cl::cat(HilappCategory));

llvm::cl::opt<bool> cmdline::comment_pragmas(
    "comment-pragmas", llvm::cl::desc("Comment out '#pragma hila' -pragmas in output"),
    llvm::cl::cat(HilappCategory));

llvm::cl::opt<bool> cmdline::insert_includes(
    "insert-includes",
    llvm::cl::desc("Insert all project #include files in .cpt -files (portable)"),
    llvm::cl::cat(HilappCategory));

llvm::cl::opt<bool> cmdline::no_include(
    "no-include",
    llvm::cl::desc("Do not insert any \'#include\'-files (for debug, may not compile)"),
    llvm::cl::cat(HilappCategory));

CompilerInstance *myCompilerInstance; // this is needed somewhere in the code
global_state global;
loop_info_struct loop_info;
codetype target; // declared extern (global)

// save original argc and argv
int cmdline::argc;
const char **cmdline::argv;

/// Check command line arguments and set appropriate flags in target
void handle_cmdline_arguments(codetype &target) {
    if (cmdline::CUDA) {
        target.CUDA = true;
    } else if (cmdline::openacc) {
        target.openacc = true;
    } else if (cmdline::AVX) {
        target.vectorize = true;
        target.vector_size = 32;
    } else if (cmdline::AVX512) {
        target.vectorize = true;
        target.vector_size = 64;
    } else if (cmdline::SSE) {
        target.vectorize = true;
        target.vector_size = 16;
    } else if (cmdline::vectorize) {
        target.vectorize = true;
        target.vector_size = cmdline::vectorize;
    }

    if (cmdline::CUDA || cmdline::openacc)
        target.is_kernelized = true;

    if (target.is_kernelized && cmdline::allow_func_globals) {
        llvm::errs() << "hilapp commandline error: kernelized target architecture '";
        if (target.CUDA)
            llvm::errs() << "cuda";
        else if (target.openacc)
            llvm::errs() << "openacc";
        llvm::errs() << "' is not compatible with option '-allow-func-globals'\n";

        exit(1);
    }
}

/////////////////////////////////////////////////////////////////////////////
/// Check allowed #pragma hila's
/////////////////////////////////////////////////////////////////////////////

/// Store #pragma hila  commands and the sourceloc where these refer to
struct pragma_loc_struct {
    SourceLocation loc, ref; // location of pragma and loc where it refers to
    pragma_hila type;        // type of pragma
    std::string arg;         // possible arg string of pragma
};

struct pragma_file_struct {
    FileID fid;
    std::vector<pragma_loc_struct> pragmas;
};

/// This holds the pragma locs, defined globally in hilapp.h
static std::vector<pragma_file_struct> pragmalocs;

// pragma_hila_types match the enum class pragma_hila

struct pragma_types {
    std::string name;
    bool has_args;
};

static std::vector<pragma_types> pragma_hila_types{
    {"skip", false},        {"ast_dump", false},     {"loop_function", false},
    {"novector", false},    {"vectorizable", false}, {"contains_rng", false},
    {"direct_access", true}};

void check_pragmas(std::string &arg, SourceLocation prloc, SourceLocation refloc,
                   std::vector<pragma_loc_struct> &pragmas) {

    std::string p = remove_initial_whitespace(arg);

    bool found = false;
    while (p.length() > 0) {

        bool found_now = false;
        for (int k = 0; k < pragma_hila_types.size(); k++) {
            auto &a = pragma_hila_types[k];

            if (contains_word_list(p, a.name, &p)) {
                // got known pragma hila
                found = found_now = true;

                pragma_loc_struct pl;
                pl.type = (pragma_hila)k;
                pl.ref = refloc;
                pl.loc = prloc;

                // any args?
                if (a.has_args) {

                    int i = 0;
                    bool error = false;

                    while (i < p.length() && std::isspace(p[i]))
                        i++;

                    if (i >= p.length() || p[i] != '(')
                        error = true;
                    else {
                        // got args, start here
                        int j = ++i;
                        int level = 1;
                        while (j < p.length() && level > 0) {
                            if (p[j] == '(')
                                level++;
                            if (p[j] == ')')
                                level--;
                            j++;
                        }
                        if (level == 0) {

                            // copy the arg, finally - now points past ), subtract it
                            pl.arg = p.substr(i, j - 2);

                            // and reset p to remaining str
                            p = p.substr(j, std::string::npos);

                        } else {

                            error = true;
                        }
                    }

                    if (error) {
                        auto &DE = myCompilerInstance->getDiagnostics();
                        auto ID = DE.getCustomDiagID(
                            DiagnosticsEngine::Level::Warning,
                            "Incorrect syntax in #pragma hila %0 arguments - ignoring\n"
                            "correct form '#pragma hila %1(<args>)");
                        auto DB = DE.Report(prloc, ID);
                        DB.AddString(a.name.c_str());
                        DB.AddString(a.name.c_str());

                        return;
                    }
                }

                // add a new pragma

                pragmas.push_back(pl);

                // exit pragma scan loop
                break;
            } // if found
        }     // for

        if (!found_now) {
            auto &DE = myCompilerInstance->getDiagnostics();
            auto ID = DE.getCustomDiagID(DiagnosticsEngine::Level::Warning,
                                         "Unknown #pragma hila -argument - ignoring");
            auto DB = DE.Report(prloc, ID);

            return;
        }

        p = remove_initial_whitespace(p);

    } // loop over whole p != 0 loop

    return; // successful return here
}

/////////////////////////////////////////////////////////////////////////////
/// Preprocessor callbacks are used to find include locs

struct includeloc_struct {
    SourceLocation HashLoc;
    StringRef FileName;
    const FileEntry *File;
    FileID fid;
    FileID fid_included;
    CharSourceRange FilenameRange;
    std::string newName;
};

// block of static vars, easiest to move information
static std::list<includeloc_struct> includelocs;

struct HILAPP_loc_struct {
    FileID fid;
    SourceRange range;
};

static std::vector<HILAPP_loc_struct> HILAPP_locs;

/// Extend PPCallbacks to handle preprocessor directives
/// specific to hila code
class MyPPCallbacks : public PPCallbacks {
  public:
    bool ifdef_HILAPP_open = false;
    SourceLocation HILAPP_sl;

    /// This hook is called when #include (or #import) is processed.
    /// It adds the file to includelocs, a list of candidates that may need to be
    /// modified and inserted into the file buffer
    void InclusionDirective(SourceLocation HashLoc, const Token &IncludeTok,
                            StringRef FileName, bool IsAngled,
                            CharSourceRange FilenameRange, const FileEntry *File,
                            StringRef SearchPath, StringRef RelativePath,
                            const Module *Imported, SrcMgr::CharacteristicKind FileType) {

        SourceManager &SM = myCompilerInstance->getSourceManager();

        if (IsAngled == false && FileType == SrcMgr::CharacteristicKind::C_User) {
            // normal user file included, add to a candidate
            includeloc_struct ci;
            ci.HashLoc = HashLoc;
            ci.FileName = FileName;
            ci.File = File;
            ci.fid = SM.getFileID(HashLoc); // FileID of the include-stmt file

            ci.FilenameRange = FilenameRange;
            ci.newName = File->tryGetRealPathName().str();

            // consider only files whose path contains "/hila/"
            if (ci.newName.find("/hila/") != std::string::npos) {
                includelocs.push_back(ci);
            }
        }
    }

    /// This is triggered when a pragma directive is encountered. It checks for the
    /// "#pragma hilapp" and stores the location, file and the command
    /// If pragma is "skip" it marks the tranlation unit for skipping.
    ///
    /// Note that pragmas where the code location is important are handled in
    /// TopLevelVisitor::has_pragma().

    void PragmaDirective(SourceLocation Loc, PragmaIntroducerKind Introducer) {
        SourceManager &SM = myCompilerInstance->getSourceManager();

        if (Introducer == clang::PIK_HashPragma) {

            // we should have #, but ensure
            if (getChar(SM, Loc) != '#')
                return;
            // skip hash, find eol
            SourceLocation sl = getNextLoc(SM, Loc);
            SourceLocation endl = findChar(SM, sl, '\n');

            if (endl.isInvalid())
                return; // should not happen

            std::string line = getRangeText(SM, sl, endl);
            std::string rest;

            if (contains_word_list(line, "pragma hila", &rest)) {
                if (contains_word_list(rest, "skip")) {
                    // OK, got #pragma hilapp skip - can quit here
                    skip_this_translation_unit = true;
                    return;
                }
                if (rest[rest.length() - 1] == '\n')
                    rest.resize(rest.length() - 1);

                FileID this_fid = SM.getFileID(Loc);

                // check if this file has previous pragmas, if not create new
                // pragma_file_struct
                int f;
                for (f = 0; f < pragmalocs.size(); f++) {
                    if (pragmalocs[f].fid == this_fid)
                        break;
                }

                if (f == pragmalocs.size()) {
                    // not file found
                    pragma_file_struct pfs;
                    pfs.fid = this_fid;
                    pragmalocs.push_back(pfs);
                    // f is correct here
                }

                // now find the SourceLocation where this #pragma should refer to.
                // skip whitespace, pragmas and #-macros
                // skip also template <> -bits
                // loop until something found
                sl = endl;

                do {
                    while (std::isspace(getChar(SM, sl)))
                        sl = getNextLoc(SM, sl);

                    if (!sl.isValid())
                        return; // File ended

                    if (getChar(SM, sl) == '#') {
                        // now pragma or macro -- skip this too
                        sl = findChar(SM, sl, '\n');
                        if (!sl.isValid())
                            return;
                    }

                    // Skip also templates
                    SourceLocation sl1;
                    if (getNextWord(SM, sl, &sl1) == "template") {
                        sl = sl1;
                        // skip template <> -brackets
                        sl = skipParens(SM, sl, '<');

                        if (!sl.isValid())
                            return;
                    }
                } while (std::isspace(getChar(SM, sl)));

                // finally, check and insert pragmas as seen
                check_pragmas(rest, Loc, sl, pragmalocs[f].pragmas);

                // llvm::errs() << " - GOT PRAGMA HILA; FILE " << f << '\n';
            }
        }
    }

    /// Also flag preprocessor directives
    /// #ifdef HILAPP .... #endif   or
    /// #ifdef HILAPP .... #else
    /// Use these for suppressing output to .cpt - it's a luxury thing, just to see that
    /// it can be done possible low-importance TODO: generalize to #if ... ?

    void Ifdef(SourceLocation Loc, const Token &MacroNameTok, const MacroDefinition &MD) {

        IdentifierInfo *ii = MacroNameTok.getIdentifierInfo();
        if (!ii)
            return;

        const char *name = ii->getNameStart();
        if (!name || strcmp(name, "HILAPP") != 0)
            return;

        // Now we have #ifdef HILAPP
        ifdef_HILAPP_open = true;
        HILAPP_sl = Loc;
    }

    /// Store the begin and end loc, and also the fid where these appear

    void is_endif_or_else(SourceLocation Loc, SourceLocation IfLoc) {

        if (!ifdef_HILAPP_open)
            return; // #ifdef HILAPP not seen
        if (IfLoc != HILAPP_sl)
            return; // some other if - endif

        SourceManager &SM = myCompilerInstance->getSourceManager();

        // and store range and fid
        HILAPP_loc_struct hloc;
        hloc.range = SourceRange(IfLoc, Loc);
        hloc.fid = SM.getFileID(IfLoc);

        ifdef_HILAPP_open = false;

        HILAPP_locs.push_back(hloc);
    }

    void Endif(SourceLocation Loc, SourceLocation IfLoc) { is_endif_or_else(Loc, IfLoc); }

    void Else(SourceLocation Loc, SourceLocation IfLoc) { is_endif_or_else(Loc, IfLoc); }

    /// This triggers when the preprocessor changes file (#include, exit from it)
    /// Use this to track the chain of non-system include files

    // void FileChanged(SourceLocation Loc, FileChangeReason Reason,
    // SrcMgr::CharacteristicKind FileType,
    //                  FileID PrevFID) {

    //   SourceManager &SM = myCompilerInstance->getSourceManager();

    //   if (Reason == PPCallbacks::EnterFile) {

    //     // entering a new file -- is this file OK?
    //     if (next_include_ok && FileType == SrcMgr::CharacteristicKind::C_User &&
    //         Loc.isValid() && !SM.isInSystemHeader(Loc)) {
    //       // Fine to include, push it on list

    //       last_ok_fid = SM.getFileID(Loc);
    //       // and note it as ok fid
    //       current_includeloc.fid_included = last_ok_fid;

    //       includelocs.push_back(current_includeloc);

    //       this_file_ok = true;

    //       llvm::errs() << "FILE CHANGED to " << SM.getFilename(Loc) << " isvalid " <<
    //       next_include_ok << '\n';
    //     } else {
    //       this_file_ok = false;
    //     }

    //   } else if (Reason == PPCallbacks::ExitFile) {

    //     FileID fid = SM.getFileID(Loc);
    //     if (this_file_ok || fid == last_ok_fid || SM.isInMainFile(Loc)) {
    //       // everything is peachy, continue - flag the this_file_ok which takes us up
    //       to main file this_file_ok = true;
    //     }
    //   }
    // }

    // /// Called for any skipped include file, mark also these points
    // ///

    // //  NOTE: LLVM 10 seems to change FileEntry -> FileEntryRef
    // //  TODO: Check the actual version, compare with 9
    // #if LLVM_VERSION_MAJOR < 10
    // void FileSkipped( const FileEntry & SkippedFile, const Token & FilenameTok,
    // 	                SrcMgr::CharacteristicKind FileType )
    // #else
    // void FileSkipped( const FileEntryRef & SkippedFile, const Token & FilenameTok,
    // 	                SrcMgr::CharacteristicKind FileType )
    // #endif
    // {
    //   SourceManager &SM = myCompilerInstance->getSourceManager();

    //   if (next_include_ok && FileType == SrcMgr::CharacteristicKind::C_User) {
    //     // this is an include candidate but skipped.  Mark the location

    //     skippedlocs.push_back(current_includeloc);

    //     llvm::errs() << "SKIPPED INCLUDE to " << current_includeloc.FileName << '\n';

    //   }
    // }

    // This triggers when range is skipped due to #if (0) .. #endif
    //   void SourceRangeSkipped(SourceRange Range, SourceLocation endLoc) {
    //     // llvm::errs() << "RANGE skipped\n";
    //   }

}; // PPCallbacks

/// General "has pragma hila" routine, can be called from any routine
/// Checks if the sourcelocation is preceded by #pragma hila
///

bool has_pragma_hila(const SourceManager &SM, SourceLocation loc, pragma_hila pragma,
                     SourceLocation &pragmaloc, const char **arg) {

    static FileID prev_fid;
    static int prev_file, prev_pragma = -1;

    // first find the listing for file
    int f;
    FileID this_fid = SM.getFileID(loc);
    if (prev_fid.isInvalid() || this_fid != prev_fid) {

        for (f = 0; f < pragmalocs.size() && pragmalocs[f].fid != this_fid; f++)
            ;
        if (f == pragmalocs.size())
            return false; // file not found
        prev_file = f;
        prev_fid = this_fid;
        prev_pragma = -1;

    } else {
        // this_fid == prev_fid
        f = prev_file;
    }

    const std::vector<pragma_loc_struct> &pl = pragmalocs[f].pragmas;
    if (pl.size() == 0)
        return false; // no pragmas after all

    // pragma search loop start
    int start = 0;

    // pragma locations are ordered in lists - start from prev found
    if (prev_pragma > 0 && pl[prev_pragma].ref <= loc) {

        // if many #pragmas on same ref, reverse these
        for (start = prev_pragma; start > 0 && pl[start].ref == loc; start--)
            ;
    }

    for (int i = start; i < pl.size() && pl[i].ref <= loc; i++) {
        if (pl[i].ref == loc && pl[i].type == pragma) {
            // found it!

            pragmaloc = pl[i].loc;

            // is arg needed?  set pointer to args, if so requested
            if (pragma_hila_types[(int)pragma].has_args && arg != nullptr) {
                *arg = pl[i].arg.c_str();
            }

            prev_pragma = i;
            return true;
        }
    }
    // no hit found, so return false
    return false;
}

/// file_buffer_list stores the edited source of all files
struct file_buffer {
    srcBuf sbuf;
    FileID fid;
};

/////////////////////////////////////////////////////////////////////////////
/// Global variable where the file buffers are hanging

std::list<file_buffer> file_buffer_list = {};

/////////////////////////////////////////////////////////////////////////////
/// get the file buffer for the file; create if it does not exist
/////////////////////////////////////////////////////////////////////////////

srcBuf *get_file_buffer(Rewriter &R, const FileID fid) {
    for (file_buffer &fb : file_buffer_list) {
        if (fb.fid == fid)
            return (&fb.sbuf);
    }
    // Now allocate and return new buffer

    SourceManager &SM = R.getSourceMgr();

    // System files should not be buffered!  FIle is OK if its name contains /hila/
    // if not found, return nullptr
    // TODO: this sounds pretty fragile!!
    std::string path = SM.getFileEntryForID(fid)->tryGetRealPathName().str();
    if (path.find("/hila/") == std::string::npos)
        return nullptr;

    file_buffer fb;
    fb.fid = fid;
    file_buffer_list.push_back(fb);
    SourceRange r(SM.getLocForStartOfFile(fid), SM.getLocForEndOfFile(fid));

    // llvm::errs() << "Create buf for file "
    //              << SM.getFilename(SM.getLocForStartOfFile(fid)) << '\n';

    file_buffer_list.back().sbuf.create(&R, r);
    return (&file_buffer_list.back().sbuf);
}

///////////////////////////////////////////////////////////////////////////////
/// Scan the list of #ifdef HILAPP -- #endif brackets and delete the code
///////////////////////////////////////////////////////////////////////////////

void remove_ifdef_HILAPP_sections() {

    /// go through all file buffers
    for (file_buffer &fb : file_buffer_list) {
        for (HILAPP_loc_struct &loc : HILAPP_locs) {
            if (loc.fid == fb.fid && fb.sbuf.is_in_range(loc.range)) {

                // got it, remove - leave #ifdef HILAPP .. #endif there to show the place
                SourceManager &SM = myCompilerInstance->getSourceManager();
                SourceLocation a = findChar(SM, loc.range.getBegin(), '\n');
                SourceLocation b = loc.range.getEnd();
                while (b.isValid() && getChar(SM, b) != '\n') {
                    b = b.getLocWithOffset(-1);
                }

                fb.sbuf.replace(SourceRange(a, b), "\n// Removed by hilapp");
            }
        }
    }
}

/////////////////////////////////////////////////////////////////////////////////
/// Implementation of the ASTConsumer interface for reading an AST produced
/// by the Clang parser.
/// This starts the main AST Visitor too
/////////////////////////////////////////////////////////////////////////////////

class MyASTConsumer : public ASTConsumer {
  public:
    MyASTConsumer(Rewriter &R, ASTContext *C) : Visitor(R, C) {}

    // HandleTranslationUnit is called after the AST for the whole TU is completed
    // Need to use this interface to ensure that specializations are present
    virtual void HandleTranslationUnit(ASTContext &ctx) override {

        SourceManager &SM = ctx.getSourceManager();
        TranslationUnitDecl *tud = ctx.getTranslationUnitDecl();
        // tud->dump();

        Visitor.reset_parsing_state();

        // Traverse each declaration in the translation unit
        for (Decl *d : tud->decls()) {

            // We only want to traverse user files. We achieve this by
            // first checking if the location of the declaration is not
            // in a system file or a virtual file.

            // Find the beginning location of the Declaration
            SourceLocation beginloc = d->getBeginLoc();
            // Macro source range doesn't point to it's actual location in the file
            // Find where the macro is called
            if (beginloc.isMacroID()) {
                Preprocessor &pp = myCompilerInstance->getPreprocessor();
                // Is there an easier way?
                CharSourceRange CSR =
                    Visitor.getRewriter().getSourceMgr().getImmediateExpansionRange(
                        beginloc);
                beginloc = CSR.getBegin();
            }

            if (!SM.isInSystemHeader(beginloc) && SM.getFilename(beginloc) != "") {

                // llvm::errs() << "Processing file " << SM.getFilename(beginloc) << "\n";
                // TODO: ensure that we go only through files which are needed!

                // get our own file edit buffer (unless it exists)
                Visitor.set_writeBuf(SM.getFileID(beginloc));

                // save this for source location
                global.location.top = d->getSourceRange().getBegin();
                global.location.bot =
                    Visitor.getSourceLocationAtEndOfRange(d->getSourceRange());

                // set the default insertion point for possibly generated kernels
                global.location.kernels = global.location.top;

                // Traverse the declaration using our AST visitor.
                // if theres "#pragma skip" don't do it
                if (!skip_this_translation_unit)
                    Visitor.TraverseDecl(d);

                // llvm::errs() << "Dumping level " << i++ << "\n";
                if (cmdline::dump_ast) {
                    if (!cmdline::no_include || SM.isInMainFile(beginloc))
                        d->dump();
                }
            }
        }

        // check compile errors, as long as we have context -- use diagnostics engine
        auto &DE = ctx.getDiagnostics();
        state::compile_errors_occurred = DE.hasErrorOccurred();
    }

  private:
    TopLevelVisitor Visitor;
};

/// This struct will be used to keep track of #include-chains.
std::vector<FileID> file_id_list = {};

/// Tiny utility to search for the list
bool search_fid(const FileID FID) {
    for (const FileID f : file_id_list) {
        if (f == FID)
            return true;
    }
    return false;
}

/// Mark a file as modified. May need to be added to the buffer.
void set_fid_modified(const FileID FID) {
    if (search_fid(FID) == false) {
        // new file to be added
        file_id_list.push_back(FID);

        SourceManager &SM = myCompilerInstance->getSourceManager();
        // llvm::errs() << "NEW BUFFER ADDED " << SM.getFileEntryForID(FID)->getName() <<
        // '\n';
    }
}

/// For each source file provided to the tool, a new FrontendAction is created.
class MyFrontendAction : public ASTFrontendAction {
  public:
    MyFrontendAction() {}

    virtual bool BeginSourceFileAction(CompilerInstance &CI) override {
        // llvm::errs() << "** Starting operation on source file "+getCurrentFile()+"\n";

        // Insert preprocessor callback functions to the stream.  This enables
        // tracking included files, ranges etc.
        Preprocessor &pp = CI.getPreprocessor();
        std::unique_ptr<MyPPCallbacks> callbacks(new MyPPCallbacks());
        pp.addPPCallbacks(std::move(callbacks));

        // init global variables PP callbacks use
        includelocs.clear();
        // clear also pragma locs and #ifdef HILAPP-locs
        pragmalocs.clear();
        HILAPP_locs.clear();

        global.main_file_name = getCurrentFile().str();

        skip_this_translation_unit = false;
        file_id_list.clear();
        file_buffer_list.clear();
        field_decl = field_storage_decl = nullptr;
        reset_vectorizable_types();
        clear_loop_functions_in_compilation_unit();

        return (true);
    }

    //////////////////////////

    int change_include_names(FileID fid) {

        int n = 0;
        srcBuf *buf = get_file_buffer(TheRewriter, fid);
        if (buf == nullptr)
            return 0;

        for (auto &inc : includelocs) {
            if (inc.fid == fid) {
                buf->replace(
                    SourceRange(inc.FilenameRange.getBegin(), inc.FilenameRange.getEnd()),
                    std::string("\"") + inc.newName + "\"");
                n++;
            }
        }
        return n;
    }

    void insert_includes_to_file_buffer(FileID myFID) {
        // this is where to write

        srcBuf *buf = get_file_buffer(TheRewriter, myFID);
        if (buf == nullptr)
            return; // system file, nothing to do

        // change filenames to be included
        change_include_names(myFID);

        // find files to be included
        SourceManager &SM = TheRewriter.getSourceMgr();

        for (FileID f : file_id_list) {

            SourceLocation IL = SM.getIncludeLoc(f);
            if (IL.isValid() && myFID == SM.getFileID(IL)) {
                // file f is included, but do #include there first
                insert_includes_to_file_buffer(f);

                // Find now '#include "file.h"' -stmt (no obv way!)
                SourceRange SR = SM.getExpansionRange(IL).getAsRange();
                SourceLocation e = SR.getEnd();
                SourceLocation b = SR.getBegin();

                // llvm::errs() << "IN file " <<  SM.getFilename(IL) << '\n';

                // Find the end of the include statement, which is an end of line
                // TODO: do this on "buf" instead of original file data
                for (int i = 1; i < 5000; i++) {
                    const char *p = SM.getCharacterData(e.getLocWithOffset(i));
                    assert(p && "Error in scanning include stmt");
                    if (*p == '\n') {
                        SR = SourceRange(b, e.getLocWithOffset(i));
                        break;
                    }
                }

                // Get the filename (the part after the includestr)
                std::string includestr = TheRewriter.getRewrittenText(SR);

                // Find the start of the include statement
                e = SR.getEnd();
                for (int i = 1; i < 5000; i++) {
                    const char *p = SM.getCharacterData(b.getLocWithOffset(-i));
                    assert(p && "Error in scanning the beginning of include stmt");
                    if (*p == '#') {
                        ++p;
                        while (std::isspace(*p))
                            ++p;
                        assert(strncmp(p, "include", 7) == 0 && "Did not find #include");
                        SR = SourceRange(b.getLocWithOffset(-i), e);
                        break;
                    }
                }

                // is the included file a system file?
                srcBuf *buf_from = get_file_buffer(TheRewriter, f);
                if (buf_from != nullptr) {

                    // Remove "#include"
                    buf->remove(SR);

                    // and finally insert
                    // SourceRange r(SM.getLocForStartOfFile(f),SM.getLocForEndOfFile(f));
                    // TheRewriter.InsertText(SR.getBegin(),
                    buf->insert(SR.getBegin(),
                                "// start include " + includestr +
                                    "---------------------------------\n" +
                                    buf_from->dump() + "// end include " + includestr +
                                    "---------------------------------\n",
                                false);
                }

            } else if (IL.isInvalid() && !SM.isInMainFile(SM.getLocForStartOfFile(f))) {
                llvm::errs() << "Invalid include loc!\n";
                llvm::errs() << "File to include: "
                             << SM.getFilename(SM.getLocForStartOfFile(f)) << '\n';

                exit(1);
            }
        }
    }

    // check and add FileID's for files in #include chains if needed
    void check_include_path(const FileID FID) {
        SourceManager &SM = TheRewriter.getSourceMgr();
        SourceLocation IL = SM.getIncludeLoc(FID);
        if (IL.isValid()) {
            FileID FID_up = SM.getFileID(IL);
            if (!search_fid(FID_up)) {
                file_id_list.push_back(FID_up);
                if (FID_up != SM.getMainFileID())
                    check_include_path(FID_up);
            }
        }
    }

    void EndSourceFileAction() override {
        SourceManager &SM = TheRewriter.getSourceMgr();
        // llvm::errs() << "** EndSourceFileAction for: " << getCurrentFile() << '\n';
        // << SM.getFileEntryForID(SM.getMainFileID())->getName() << "\n";

        // Now emit rewritten buffers.

        if (!cmdline::no_output) {

            remove_ifdef_HILAPP_sections();

            if (!cmdline::no_include) {

                // Modified files should be substituted on top of #include -directives
                // First, find buffers which are modified

                file_id_list.clear();

                for (file_buffer &fb : file_buffer_list) {
                    if (fb.sbuf.is_modified() || cmdline::insert_includes)
                        set_fid_modified(fb.fid);
                }

                // then, ensure that the full include chain is present in file_id_list
                // Use iterator here, because the list can grow!

                for (int fi = 0; fi < file_id_list.size(); fi++) {
                    FileID f = file_id_list[fi];
                    // llvm::errs() << "Checking file "
                    //      << SM.getFilename(SM.getLocForStartOfFile(f)) << '\n';
                    check_include_path(f);
                }

                //        change_include_names(SM.getMainFileID());

                insert_includes_to_file_buffer(SM.getMainFileID());
            }

            if (!state::compile_errors_occurred) {
                write_output_file(
                    cmdline::output_filename,
                    get_file_buffer(TheRewriter, SM.getMainFileID())->dump());

                if (cmdline::function_spec_no_inline || cmdline::method_spec_no_inline)
                    write_specialization_db();
            } else {
                llvm::errs() << program_name
                             << ": not writing output due to compile errors\n";
            }
        }

        file_buffer_list.clear();
        file_id_list.clear();

        // EndSourceFile();
    }

    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                   StringRef file) override {
        // llvm::errs() << "** Creating AST consumer for: " << file << "\n";
        TheRewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
        myCompilerInstance = &CI;
#if defined(__clang_major__) && (__clang_major__ <= 9)
        return llvm::make_unique<MyASTConsumer>(TheRewriter, &CI.getASTContext());
#else
        return std::make_unique<MyASTConsumer>(TheRewriter, &CI.getASTContext());
#endif
    }

  private:
    Rewriter TheRewriter;
    // ASTContext  TheContext;
};

/////////////////////////////////////////////////////////////////////////////////////
/// Main routine
/////////////////////////////////////////////////////////////////////////////////////

int main(int argc, const char **argv) {

    // TODO: clang CommandLine.cpp/.h has strange category and help
    // msg handling, should we get rid of it?

    cmdline::argc = argc;
    cmdline::argv = argv;

    // av takes over from argv
    const char **av = new const char *[argc + 6];
    argc = rearrange_cmdline(argc, argv, av);
    av[argc++] = "-std=c++17"; // use c++17 std
    av[argc++] = "-DHILAPP";   // add global defn
    av[argc] = nullptr;

    OptionsParser op(argc, av, HilappCategory);
    ClangTool Tool(op.getCompilations(), op.getSourcePathList());

    // We have command line args, possibly do something with them
    handle_cmdline_arguments(target);
    if (cmdline::syntax_only)
        cmdline::no_output = true;

    // ClangTool::run accepts a FrontendActionFactory, which is then used to
    // create new objects implementing the FrontendAction interface. Here we use
    // the helper newFrontendActionFactory to create a default factory that will
    // return a new MyFrontendAction object every time.
    // To further customize this, we could create our own factory class.
    return Tool.run(newFrontendActionFactory<MyFrontendAction>().get());
}

/////////////////////////////////////////////////////////////////////////////////////
/// Some sourceloc utilities
/////////////////////////////////////////////////////////////////////////////////////

/// Get next character and sourcelocation, while skipping comments.
/// On line comments return the eol char

SourceLocation getNextLoc(const SourceManager &SM, SourceLocation sl, bool forward) {
    bool invalid = false;

    int dir;
    if (forward)
        dir = 1;
    else
        dir = -1;
    SourceLocation s = sl.getLocWithOffset(dir);
    const char *c = SM.getCharacterData(s, &invalid);

    // skip comments - only c-style backwards
    while ('/' == *c) {

        if (forward && '/' == *SM.getCharacterData(s.getLocWithOffset(1), &invalid)) {
            // a comment, skip the rest of line
            while (!invalid && *SM.getCharacterData(s, &invalid) != '\n')
                s = s.getLocWithOffset(1);
            c = SM.getCharacterData(s, &invalid);

        } else if ('*' == *SM.getCharacterData(s.getLocWithOffset(dir), &invalid)) {
            // c-style comment
            s = s.getLocWithOffset(2 * dir);
            while (!invalid && *SM.getCharacterData(s, &invalid) != '*' &&
                   *SM.getCharacterData(s.getLocWithOffset(dir), &invalid) != '/')
                s = s.getLocWithOffset(dir);
            s = s.getLocWithOffset(2 * dir);
            c = SM.getCharacterData(s, &invalid);
        } else
            break; // exit from here
    }

    return s;
}

char getChar(const SourceManager &SM, SourceLocation sl) {
    bool invalid = false;
    const char *c = SM.getCharacterData(sl, &invalid);
    if (invalid)
        return 0;
    else
        return *c;
}

// Find the location of the next searched for char.
SourceLocation findChar(const SourceManager &SM, SourceLocation sloc, char ct) {
    bool invalid = false;
    while (sloc.isValid()) {
        const char *c = SM.getCharacterData(sloc, &invalid);
        if (*c == ct)
            return sloc;
        sloc = getNextLoc(SM, sloc);
    }
    return sloc;
}

/// Skip paren expression following sl, points after the paren
/// If partype == '(', just balance par expressions (may contain strings).
/// If partype == '<', balance < > -parens (may contain () -parens, which are balanced in
/// turn) This last one is useful for template scanning

SourceLocation skipParens(const SourceManager &SM, SourceLocation sl,
                          const char partype) {

    assert((partype == '(' || partype == '<') && "Unknown paren type in skipParens");

    char endpar;
    if (partype == '<')
        endpar = '>';
    else
        endpar = ')';

    while (sl.isValid() && getChar(SM, sl) != partype)
        sl = getNextLoc(SM, sl);

    sl = getNextLoc(SM, sl);
    char c = getChar(SM, sl);
    while (sl.isValid() && c != endpar) {

        if (c == partype) {
            sl = skipParens(SM, sl, partype);
        } else if (c == '(') {
            sl = skipParens(SM, sl);
        } else if (c == '"' || c == '\'') {
            sl = skipString(SM, sl);
        } else {
            // default option, next char loc
            sl = getNextLoc(SM, sl);
        }

        // and check next char
        c = getChar(SM, sl);
    }

    // successful exit here
    if (c == endpar)
        return getNextLoc(SM, sl);
    // this must be !isValid()
    return sl;
}

/// Skip " " or ' ' after location, returns the next location

SourceLocation skipString(const SourceManager &SM, SourceLocation sl) {

    char c;
    do {
        c = getChar(SM, sl);
        sl = getNextLoc(SM, sl);
    } while (sl.isValid() && c != '\'' && c != '"');

    // first single char
    if (c == '\'') {
        c = getChar(SM, sl);
        if (c == '\\')
            sl = sl.getLocWithOffset(1);
        sl = sl.getLocWithOffset(2); // jumps over the closing '
        return sl;
    }

    // now string " "
    int lev = 1;
    while (lev > 0 && sl.isValid()) {
        char c = getChar(SM, sl);
        if (c == '\\')
            sl = sl.getLocWithOffset(1);
        if (c == '"')
            lev = 0;
        sl = sl.getLocWithOffset(1);
    }

    return sl;
}

/// Get next word starting from sl -- if end is non-null, return the end of the
/// word string here (points to last char)

std::string getNextWord(const SourceManager &SM, SourceLocation sl, SourceLocation *end) {
    while (std::isspace(getChar(SM, sl)))
        sl = getNextLoc(SM, sl); // skip spaces

    std::string res;
    SourceLocation endloc = sl;
    char c = getChar(SM, sl);
    if (std::isalnum(c) || c == '_') {
        while (sl.isValid() && (std::isalnum(c) || c == '_')) {
            res.push_back(c);
            endloc = sl;
            sl = getNextLoc(SM, sl);
            c = getChar(SM, sl);
        }
    } else
        res.push_back(c);
    if (end != nullptr)
        *end = endloc;
    return res;
}

/// Return the text within the range, inclusive (note - skip comments, as usual)
std::string getRangeText(const SourceManager &SM, SourceLocation begin,
                         SourceLocation end) {

    // need to be in the same file - and end needs to be after begin
    if (SM.getFileID(begin) != SM.getFileID(end) || begin > end)
        return "";

    std::string res;
    do {
        res.push_back(getChar(SM, begin));
        begin = getNextLoc(SM, begin);
    } while (begin <= end);
    return res;
}
