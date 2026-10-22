package me.magnum.melonds.ui.settings.fragments

import android.content.Intent
import android.os.Bundle
import android.text.format.Formatter
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.preference.Preference
import androidx.preference.MultiSelectListPreference
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.magnum.melonds.domain.repositories.LayoutsRepository
import androidx.preference.PreferenceFragmentCompat
import androidx.preference.SeekBarPreference
import androidx.preference.SwitchPreference
import dagger.hilt.android.AndroidEntryPoint
import me.magnum.melonds.R
import me.magnum.melonds.common.DirectoryAccessValidator
import me.magnum.melonds.common.UriPermissionManager
import me.magnum.melonds.common.vibration.TouchVibrator
import me.magnum.melonds.extensions.addOnPreferenceChangeListener
import me.magnum.melonds.impl.recording.RecordingFileManager
import me.magnum.melonds.ui.inputsetup.InputSetupActivity
import me.magnum.melonds.ui.layouts.LayoutListActivity
import me.magnum.melonds.ui.settings.PreferenceFragmentHelper
import me.magnum.melonds.ui.settings.PreferenceFragmentTitleProvider
import me.magnum.melonds.ui.settings.SettingsActivity
import me.magnum.melonds.ui.settings.preferences.InGameLockedPreference
import me.magnum.melonds.ui.settings.preferences.SoftwareInputBehaviourPreference
import me.magnum.melonds.ui.settings.preferences.StoragePickerPreference
import javax.inject.Inject

@AndroidEntryPoint
class InputPreferencesFragment : BasePreferenceFragment(), PreferenceFragmentTitleProvider {

    @Inject lateinit var vibrator: TouchVibrator
    @Inject lateinit var layoutsRepository: LayoutsRepository
    @Inject lateinit var uriPermissionManager: UriPermissionManager
    @Inject lateinit var directoryAccessValidator: DirectoryAccessValidator
    @Inject lateinit var recordingFileManager: RecordingFileManager

    private val helper by lazy { PreferenceFragmentHelper(this, uriPermissionManager, directoryAccessValidator) }

    private lateinit var softInputBehaviourPreference: SoftwareInputBehaviourPreference

    override fun getTitle() = getString(R.string.input)

    override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
        setPreferencesFromResource(R.xml.pref_input, rootKey)
        softInputBehaviourPreference = findPreference("soft_input_behaviour")!!
        val cycleLayoutsPreference = findPreference<MultiSelectListPreference>("input_cycle_layout_ids")!!
        cycleLayoutsPreference.isPersistent = false
        cycleLayoutsPreference.setOnPreferenceChangeListener { _, value ->
            @Suppress("UNCHECKED_CAST")
            val selectedIds = value as Set<String>
            cycleLayoutsPreference.sharedPreferences?.edit()
                ?.putStringSet(cycleLayoutsPreference.key, selectedIds)?.apply()
            true
        }
        val touchVibratePreference = findPreference<SwitchPreference>("input_touch_haptic_feedback_enabled")!!
        val vibrationStrengthPreference = findPreference<SeekBarPreference>("input_touch_haptic_feedback_strength")!!
        val keyMappingPreference = findPreference<InGameLockedPreference>("input_key_mapping")!!
        val layoutsPreference = findPreference<InGameLockedPreference>("input_layouts")!!
        keyMappingPreference.isInGameLocked = requireActivity().intent.getBooleanExtra(SettingsActivity.KEY_LOCK_INPUT_MAPPING, false)
        keyMappingPreference.inGameLockedMessageRes = R.string.cannot_change_use_rom_settings
        layoutsPreference.isInGameLocked = requireActivity().intent.getBooleanExtra(SettingsActivity.KEY_LOCK_INPUT_LAYOUT, false)
        layoutsPreference.inGameLockedMessageRes = R.string.cannot_change_use_rom_settings

        if (!vibrator.supportsVibration()) {
            touchVibratePreference.isVisible = false
        }
        vibrationStrengthPreference.isVisible = false

        vibrationStrengthPreference.setOnPreferenceChangeListener { _, newValue ->
            val strength = newValue as Int
            vibrator.performTouchHapticFeedback(strength)
            true
        }
        keyMappingPreference.setOnPreferenceClickListener {
            val intent = InputSetupActivity.getGlobalIntent(requireContext())
            startActivity(intent)
            true
        }
        layoutsPreference.setOnPreferenceClickListener {
            val intent = Intent(requireContext(), LayoutListActivity::class.java)
            startActivity(intent)
            true
        }

        val recordingDirectory = findPreference<StoragePickerPreference>("recording_directory")!!
        helper.setupStoragePickerPreference(recordingDirectory)
        recordingDirectory.addOnPreferenceChangeListener { _, value ->
            if ((value as? Set<*>)?.isNotEmpty() != true) {
                recordingDirectory.summary = getString(R.string.recording_directory_default)
            }
            true
        }
        if (recordingDirectory.getPersistedStringSet(null).isNullOrEmpty()) {
            recordingDirectory.summary = getString(R.string.recording_directory_default)
        }

        findPreference<Preference>("delete_unsaved_recordings")!!.setOnPreferenceClickListener {
            confirmDeleteUnsavedRecordings()
            true
        }
    }

    private fun updateUnsavedRecordingsSummary() {
        lifecycleScope.launch {
            val leftovers = withContext(Dispatchers.IO) { recordingFileManager.getLeftoverFiles() }
            val preference = findPreference<Preference>("delete_unsaved_recordings") ?: return@launch
            preference.summary = if (leftovers.count == 0) {
                getString(R.string.delete_unsaved_recordings_none)
            } else {
                getString(R.string.delete_unsaved_recordings_summary, leftovers.count, Formatter.formatShortFileSize(requireContext(), leftovers.totalBytes))
            }
        }
    }

    private fun confirmDeleteUnsavedRecordings() {
        lifecycleScope.launch {
            val leftovers = withContext(Dispatchers.IO) { recordingFileManager.getLeftoverFiles() }
            if (leftovers.count == 0) {
                Toast.makeText(requireContext(), R.string.delete_unsaved_recordings_none, Toast.LENGTH_SHORT).show()
                updateUnsavedRecordingsSummary()
                return@launch
            }
            AlertDialog.Builder(requireContext())
                .setTitle(R.string.delete_unsaved_recordings)
                .setMessage(getString(R.string.delete_unsaved_recordings_confirmation, leftovers.count, Formatter.formatShortFileSize(requireContext(), leftovers.totalBytes)))
                .setPositiveButton(R.string.delete) { _, _ ->
                    lifecycleScope.launch {
                        val deleted = withContext(Dispatchers.IO) { recordingFileManager.deleteLeftoverFiles() }
                        Toast.makeText(requireContext(), getString(R.string.delete_unsaved_recordings_done, deleted.count), Toast.LENGTH_SHORT).show()
                        updateUnsavedRecordingsSummary()
                    }
                }
                .setNegativeButton(R.string.cancel, null)
                .show()
        }
    }

    override fun onResume() {
        super.onResume()
        lifecycleScope.launch {
            val layouts = layoutsRepository.getLayouts().first().filter { it.id != null }
            val preference = findPreference<MultiSelectListPreference>("input_cycle_layout_ids") ?: return@launch
            val ids = layouts.map { it.id.toString() }.toTypedArray()
            preference.entries = layouts.map { it.name ?: getString(R.string.custom_layout_default_name) }.toTypedArray()
            preference.entryValues = ids
            preference.values = preference.sharedPreferences
                ?.getStringSet(preference.key, null)?.intersect(ids.toSet()) ?: ids.toSet()
        }
        updateUnsavedRecordingsSummary()
        // Set proper value for soft input behaviour preference since the value is not updated when returning from the fragment
        softInputBehaviourPreference.value = softInputBehaviourPreference.sharedPreferences?.getString(softInputBehaviourPreference.key, "hide_system_buttons_when_controller_connected")
    }
}
