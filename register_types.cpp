/*************************************************************************/
/*  register_types.cpp                                                   */
/*************************************************************************/

#include "register_types.h"
#include "compat/fake_script.h"
#include "core/object/class_db.h"
#include "modules/regex/regex.h"
#include "utility/file_access_gdre.h"
#include "utility/gdre_standalone.h"
#include "utility/text_diff.h"
#ifdef TOOLS_ENABLED
#include "editor/editor_node.h"
#endif

#include "bytecode/bytecode_versions.h"
#include "compat/resource_compat_binary.h"
#include "compat/resource_compat_text.h"
#include "compat/resource_loader_compat.h"
#include "compat/script_loader.h"
#include "exporters/export_report.h"
#include "exporters/resource_exporter.h"
#include "exporters/translation_exporter.h"
#include "utility/common.h"
#include "utility/gdre_config.h"
#include "utility/gdre_settings.h"
#include "utility/glob.h"
#include "utility/godotver.h"
#include "utility/import_exporter.h"
#include "utility/packed_file_info.h"
#include "utility/pck_creator.h"
#include "utility/pck_dumper.h"
#include "utility/task_manager.h"
#include "utility/translation_converter.h"

#ifdef TOOLS_ENABLED
void gdsdecomp_init_callback() {
}
#endif

static GDRESettings *gdre_singleton = nullptr;
static TaskManager *task_manager = nullptr;
static GDREConfig *gdre_config = nullptr;
// TODO: move this to its own thing
static Ref<ResourceFormatLoaderCompatText> text_loader = nullptr;
static Ref<ResourceFormatLoaderCompatBinary> binary_loader = nullptr;
static Ref<ResourceFormatGDScriptLoader> script_loader = nullptr;

//exporters
static Ref<TranslationExporter> translation_exporter = nullptr;

void init_ver_regex() {
	SemVer::strict_regex = RegEx::create_from_string(GodotVer::strict_regex_str);
	GodotVer::non_strict_regex = RegEx::create_from_string(GodotVer::non_strict_regex_str);
	Glob::magic_check = RegEx::create_from_string(Glob::magic_pattern);
	Glob::escapere = RegEx::create_from_string(Glob::escape_pattern);
}

void free_ver_regex() {
	SemVer::strict_regex = Ref<RegEx>();
	GodotVer::non_strict_regex = Ref<RegEx>();
	Glob::magic_check = Ref<RegEx>();
	Glob::escapere = Ref<RegEx>();
}

void init_loaders() {
	text_loader = memnew(ResourceFormatLoaderCompatText);
	binary_loader = memnew(ResourceFormatLoaderCompatBinary);
	script_loader = memnew(ResourceFormatGDScriptLoader);
	ResourceCompatLoader::add_resource_format_loader(binary_loader, true);
	ResourceCompatLoader::add_resource_format_loader(text_loader, true);
	ResourceCompatLoader::add_resource_format_loader(script_loader, true);
}

void init_exporters() {
	translation_exporter = memnew(TranslationExporter);

	Exporter::add_exporter(translation_exporter);
}

void init_plugin_manager_sources() {
}

void deinit_plugin_manager_sources() {
}

void deinit_exporters() {
	if (translation_exporter.is_valid()) {
		Exporter::remove_exporter(translation_exporter);
	}
	translation_exporter = nullptr;
}

void deinit_loaders() {
	if (text_loader.is_valid()) {
		ResourceCompatLoader::remove_resource_format_loader(text_loader);
	}
	if (binary_loader.is_valid()) {
		ResourceCompatLoader::remove_resource_format_loader(binary_loader);
	}
	if (script_loader.is_valid()) {
		ResourceCompatLoader::remove_resource_format_loader(script_loader);
	}
	text_loader = nullptr;
	binary_loader = nullptr;
	script_loader = nullptr;
}

void initialize_gdtr_module(ModuleInitializationLevel p_level) {
	ResourceLoader::set_create_missing_resources_if_class_unavailable(true);
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	ClassDB::register_class<SemVer>();
	ClassDB::register_class<GodotVer>();
	ClassDB::register_class<Glob>();
	init_ver_regex();

	ClassDB::register_abstract_class<GDScriptDecomp>();
	register_decomp_versions();

	ClassDB::register_class<FileAccessGDRE>();

	ClassDB::register_class<GodotREEditorStandalone>();
	ClassDB::register_class<PckDumper>();
	ClassDB::register_class<PckCreator>();
	ClassDB::register_class<ResourceImportMetadatav2>();
	ClassDB::register_abstract_class<ImportInfo>();
	ClassDB::register_class<ProjectConfigLoader>();
	ClassDB::register_class<TranslationConverter>();

	ClassDB::register_class<Exporter>();
	ClassDB::register_class<ExportReport>();
	ClassDB::register_class<ResourceExporter>();
	ClassDB::register_class<TranslationExporter>();
	ClassDB::register_class<ResourceCompatLoader>();
	ClassDB::register_class<CompatFormatLoader>();
	ClassDB::register_class<ResourceFormatLoaderCompatText>();
	ClassDB::register_class<ResourceFormatLoaderCompatBinary>();
	ClassDB::register_class<ResourceFormatGDScriptLoader>();
	// TODO: make ResourceCompatConverter non-abstract
	ClassDB::register_abstract_class<ResourceCompatConverter>();
	ClassDB::register_class<FakeEmbeddedScript>();
	ClassDB::register_class<FakeGDScript>();
	ClassDB::register_class<ImportInfoModern>();
	ClassDB::register_class<ImportInfov2>();
	ClassDB::register_class<ImportInfoDummy>();
	ClassDB::register_class<ImportInfoRemap>();
	ClassDB::register_class<ImportInfoGDExt>();
	ClassDB::register_class<ImportExporter>();
	ClassDB::register_class<ImportExporterReport>();
	ClassDB::register_class<GDRESettings>();

	ClassDB::register_class<PackedFileInfo>();
	ClassDB::register_class<GDRESettings::PackInfo>();

	ClassDB::register_class<GDRECommon>();
	ClassDB::register_class<TextDiff>();
	ClassDB::register_class<TaskManager>();
	ClassDB::register_class<ResourceInfo>();

	ClassDB::register_class<GDREConfig>();
	ClassDB::register_class<GDREConfigSetting>();

	init_plugin_manager_sources();
	gdre_singleton = memnew(GDRESettings);
	Engine::get_singleton()->add_singleton(Engine::Singleton("GDRESettings", GDRESettings::get_singleton()));
	gdre_config = memnew(GDREConfig);
	Engine::get_singleton()->add_singleton(Engine::Singleton("GDREConfig", GDREConfig::get_singleton()));
	task_manager = memnew(TaskManager);
	Engine::get_singleton()->add_singleton(Engine::Singleton("TaskManager", TaskManager::get_singleton()));
#ifdef TOOLS_ENABLED
	EditorNode::add_init_callback(&gdsdecomp_init_callback);
#endif
	init_loaders();
	init_exporters();
}

void uninitialize_gdtr_module(ModuleInitializationLevel p_level) {
	deinit_exporters();
	deinit_loaders();
	if (gdre_config) {
		memdelete(gdre_config);
		gdre_config = nullptr;
	}
	if (gdre_singleton) {
		memdelete(gdre_singleton);
		gdre_singleton = nullptr;
	}
	if (task_manager) {
		memdelete(task_manager);
		task_manager = nullptr;
	}
	deinit_plugin_manager_sources();
	free_ver_regex();
}
