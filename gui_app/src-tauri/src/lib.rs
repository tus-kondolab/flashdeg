pub mod commands;
pub mod featurecounts;
pub mod flashdeg;
pub mod paths;
pub mod project;
pub mod results;
pub mod validation;

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_fs::init())
        .manage(flashdeg::RunRegistry::default())
        .invoke_handler(tauri::generate_handler![
            commands::default_project,
            commands::load_project,
            commands::save_project,
            commands::load_results_csv,
            commands::validate_inputs,
            commands::inspect_metadata,
            commands::locate_flashdeg,
            commands::launch_new_instance,
            commands::read_gene_map,
            commands::read_gene_map_file,
            commands::save_plot,
            commands::copy_svg,
            commands::validate_extra_args,
            commands::run_analysis,
            commands::cancel_analysis,
            commands::inspect_featurecounts,
            commands::merge_featurecounts,
            commands::write_metadata_csv,
        ])
        .run(tauri::generate_context!())
        .expect("error while running FlashDEG GUI");
}
