"""Validation de la configuration ESPHome (`esphome config`).

L'exemple est copié dans un dossier temporaire avec des secrets fictifs générés
à la volée : aucun identifiant n'est écrit dans le dépôt.
"""

import ast
import base64
import os
import re
import subprocess
from pathlib import Path

import pytest

EXAMPLE = "examples/iot-viessmann.yaml"
# Autorisations d'écriture : actives par défaut, `false` pour désactiver.
WRITE_FLAGS = (
    "enable_writes",
    "enable_mode_writes",
    "enable_m2_mode_write",
    "enable_schedule_writes",
    "enable_dhw_setpoint_write",
    "enable_heating_setpoint_writes",
)
# Jours du bouclage : les sept par défaut.
SEVEN_DAYS = (
    "monday",
    "tuesday",
    "wednesday",
    "thursday",
    "friday",
    "saturday",
    "sunday",
)
# Exemples de la v0.1.0 et de la v0.2, tels que publiés : doivent rester
# valides sans modification.
EXAMPLE_V01 = "tests/fixtures/iot-viessmann-v0.1.yaml"
EXAMPLE_V02 = "tests/fixtures/iot-viessmann-v0.2.yaml"
# v0.2.2 : configuration de diagnostic (exemple + six registres TEST bruts).
DIAGNOSTIC = "examples/iot-viessmann-diagnostic.yaml"
# Exemple de production de la v0.2.2 (identique à la v0.2.1) et diagnostic,
# tels que publiés.
EXAMPLE_V022 = "tests/fixtures/iot-viessmann-v0.2.2.yaml"
DIAGNOSTIC_V022 = "tests/fixtures/iot-viessmann-v0.2.2-diagnostic.yaml"
# Exemple de la v0.3 (A1 en select, M2 en text_sensor), tel que publié.
EXAMPLE_V03 = "tests/fixtures/iot-viessmann-v0.3.yaml"
# Exemple de la v0.4 (deux select, trois autorisations), tel que publié.
EXAMPLE_V04 = "tests/fixtures/iot-viessmann-v0.4.yaml"
# Exemple de la v0.4.1, tel que publié.
EXAMPLE_V041 = "tests/fixtures/iot-viessmann-v0.4.1.yaml"


def _secrets() -> str:
    key = base64.b64encode(os.urandom(32)).decode()
    return (
        'wifi_ssid: "test"\n'
        'wifi_password: "test-password"\n'
        'fallback_ap_password: "test-fallback"\n'
        f'api_encryption_key: "{key}"\n'
    )


def _with_flags(**flags):
    """Transformation : déclare explicitement des autorisations sous
    `visslink:` (les exemples n'en déclarent plus : défaut true)."""

    def transform(text):
        lines = "".join(f"\n  {k}: {v}" for k, v in flags.items())
        new, n = re.subn(
            r"(\n  max_retries: [^\n]*)", lambda m: m.group(1) + lines, text, 1
        )
        assert n == 1
        return new

    return transform


def _run_config(repo_root, tmp_path, transform=lambda s: s, example=EXAMPLE):
    text = (repo_root / example).read_text(encoding="utf-8")
    text = text.replace("path: ../components", f"path: {repo_root / 'components'}")
    text = transform(text)
    (tmp_path / "secrets.yaml").write_text(_secrets(), encoding="utf-8")
    config = tmp_path / "test.yaml"
    config.write_text(text, encoding="utf-8")
    return subprocess.run(
        ["esphome", "config", str(config)],
        check=False,
        capture_output=True,
        text=True,
        cwd=tmp_path,
    )


def test_example_config_is_valid(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "Configuration is valid" in result.stdout + result.stderr


def test_example_has_no_hardcoded_secrets(repo_root):
    text = (repo_root / EXAMPLE).read_text(encoding="utf-8")
    for key in ("password:", "key:"):
        for line in text.splitlines():
            if line.strip().startswith(key):
                assert "!secret" in line, line


INVALID_CASES = {
    "wrong_baud_rate": lambda s: s.replace("baud_rate: 4800", "baud_rate: 9600"),
    "wrong_parity": lambda s: s.replace("parity: EVEN", "parity: NONE"),
    "wrong_stop_bits": lambda s: s.replace("stop_bits: 2", "stop_bits: 1"),
    # Option retirée : le capteur lit toujours un int16 de 2 octets.
    "unknown_length_option": lambda s: s.replace(
        "address: 0x0800", "address: 0x0800\n    length: 2"
    ),
    "interval_too_short": lambda s: s.replace(
        "update_interval: 60s", "update_interval: 500ms", 1
    ),
    "interval_too_long": lambda s: s.replace(
        "update_interval: 60s", "update_interval: 30d", 1
    ),
    "response_timeout_above_sync_period": lambda s: s.replace(
        "response_timeout: 1s", "response_timeout: 2s"
    ),
    "duplicate_register": lambda s: s.replace("address: 0x0804", "address: 0x0800"),
    # v0.2 : types de données et binary_sensor.
    "unknown_data_type": lambda s: s.replace(
        "data_type: int16", "data_type: float32", 1
    ),
    "signed_32_bit_not_supported": lambda s: s.replace(
        "data_type: uint32", "data_type: int32", 1
    ),
    "data_type_on_binary_sensor": lambda s: s.replace(
        "address: 0x0842", "address: 0x0842\n    data_type: uint8"
    ),
    "length_on_binary_sensor": lambda s: s.replace(
        "address: 0x0842", "address: 0x0842\n    length: 1"
    ),
    "binary_interval_too_short": lambda s: s.replace(
        "update_interval: 30s", "update_interval: 100ms"
    ),
    # Un registre n'est lu qu'une fois, toutes plateformes confondues.
    "duplicate_register_across_platforms": lambda s: s.replace(
        "address: 0x0842", "address: 0x0800"
    ),
    "address_out_of_range": lambda s: s.replace("address: 0x0804", "address: 0x10000"),
    # v0.2.1 : programmations horaires (text_sensor).
    # Les 7 blocs comptent : 0x2008 est le mardi de 0x2000.
    "sensor_inside_schedule_blocks": lambda s: s.replace(
        "address: 0x0804", "address: 0x2008"
    ),
    "binary_sensor_on_schedule_sunday": lambda s: s.replace(
        "address: 0x0842", "address: 0x2230"
    ),
    "overlapping_schedules": lambda s: s.replace("address: 0x3000", "address: 0x2030"),
    "same_schedule_twice": lambda s: s.replace("address: 0x2100", "address: 0x2000"),
    "schedule_address_too_high": lambda s: s.replace(
        "address: 0x2200", "address: 0xFFD0"
    ),
    "schedule_interval_too_long": lambda s: s.replace(
        "update_interval: 6h", "update_interval: 8d", 1
    ),
    "today_and_active_without_time": lambda s: s.replace(
        "    time_id: ha_time\n", "", 1
    ),
    "active_without_time": lambda s: _drop_first(
        _drop_first(s, "    time_id: ha_time\n"),
        '    today:\n      name: "Radiateurs - Programmation du jour"\n',
    ),
    "data_type_on_schedule": lambda s: s.replace(
        "address: 0x2000", "address: 0x2000\n    data_type: uint8"
    ),
    "length_on_schedule": lambda s: s.replace(
        "address: 0x2000", "address: 0x2000\n    length: 8"
    ),
    "unknown_day_key": lambda s: s.replace(
        "address: 0x2000", 'address: 0x2000\n    lundi:\n      name: "Lundi"'
    ),
    # v0.3 : consignes (number) et écritures.
    "enable_writes_not_boolean": lambda s: _with_flags(enable_writes="maybe")(s),
    "number_address_not_whitelisted": lambda s: s.replace(
        "address: 0x2306", "address: 0x0846"
    ),
    "number_on_burner_register": lambda s: s.replace(
        "address: 0x2306", "address: 0x0842"
    ),
    "number_on_temperature_register": lambda s: s.replace(
        "address: 0x2306", "address: 0x0800"
    ),
    "same_setpoint_twice": lambda s: s.replace("address: 0x2307", "address: 0x2306"),
    "setpoint_also_declared_as_sensor": lambda s: s.replace(
        "address: 0x0804", "address: 0x6300"
    ),
    "number_min_below_documented": lambda s: s.replace(
        "min_value: 16", "min_value: 2", 1
    ),
    "number_max_above_documented": lambda s: s.replace(
        "max_value: 24", "max_value: 38", 1
    ),
    "dhw_max_above_documented": lambda s: s.replace("max_value: 60", "max_value: 61"),
    "dhw_min_below_documented": lambda s: s.replace("min_value: 40", "min_value: 9"),
    "number_min_not_below_max": lambda s: s.replace(
        "min_value: 16", "min_value: 24", 1
    ),
    "number_fractional_bound": lambda s: s.replace(
        "min_value: 16", "min_value: 16.5", 1
    ),
    "number_half_step": lambda s: s.replace("step: 1", "step: 0.5", 1),
    "number_step_two": lambda s: s.replace("step: 1", "step: 2", 1),
    "number_without_min_value": lambda s: s.replace("    min_value: 16\n", "", 1),
    "number_without_max_value": lambda s: s.replace("    max_value: 24\n", "", 1),
    "number_with_data_type": lambda s: s.replace(
        "address: 0x2306", "address: 0x2306\n    data_type: uint16"
    ),
    "number_with_restore_value": lambda s: s.replace(
        "address: 0x2306", "address: 0x2306\n    restore_value: true"
    ),
    "number_with_initial_value": lambda s: s.replace(
        "address: 0x2306", "address: 0x2306\n    initial_value: 20"
    ),
    # v0.3/v0.4 : modes de fonctionnement (deux select).
    # Contradiction explicite : true sous un parent explicitement false.
    "mode_writes_without_writes": lambda s: _with_flags(
        enable_writes="false", enable_mode_writes="true"
    )(s),
    "mode_writes_not_boolean": lambda s: _with_flags(enable_mode_writes="perhaps")(s),
    # v0.4 : M2 exige enable_writes ET enable_mode_writes.
    "m2_write_alone": lambda s: _with_flags(
        enable_writes="false", enable_mode_writes="false", enable_m2_mode_write="true"
    )(s),
    "m2_write_with_writes_only": lambda s: _with_flags(
        enable_mode_writes="false", enable_m2_mode_write="true"
    )(s),
    "m2_write_with_mode_writes_only": lambda s: _with_flags(
        enable_writes="false", enable_m2_mode_write="true"
    )(s),
    "m2_write_not_boolean": lambda s: _with_flags(enable_m2_mode_write="maybe")(s),
    "select_on_party_register": lambda s: s.replace(
        "address: 0x2301", "address: 0x2303"
    ),
    "select_on_candidate_2323": lambda s: s.replace(
        "address: 0x2301", "address: 0x2323"
    ),
    "select_on_candidate_3323": lambda s: s.replace(
        "address: 0x3301", "address: 0x3323"
    ),
    "select_on_setpoint": lambda s: s.replace("address: 0x2301", "address: 0x2307"),
    "select_on_schedule": lambda s: s.replace("address: 0x3301", "address: 0x2000"),
    "same_mode_register_twice": lambda s: s.replace(
        "address: 0x3301", "address: 0x2301"
    ),
    "mode_register_also_sensor": lambda s: s.replace(
        "address: 0x0804", "address: 0x3301"
    ),
    "mode_register_also_text_sensor": lambda s: (
        s
        + "\n  - platform: visslink\n    type: operating_mode\n"
        + '    name: "M2 bis"\n    address: 0x3301\n'
    ),
    "mode_text_on_setpoint": lambda s: (
        s
        + "\n  - platform: visslink\n    type: operating_mode\n"
        + '    name: "X"\n    address: 0x3306\n'
    ),
    "unknown_text_sensor_type": lambda s: _m2_as_text(s, type_="mode"),
    "mode_text_with_today": lambda s: _m2_as_text(
        s, extra='    today:\n      name: "Y"\n'
    ),
    "select_with_options": lambda s: s.replace(
        "address: 0x2301", 'address: 0x2301\n    options: ["Party"]'
    ),
    "select_with_data_type": lambda s: s.replace(
        "address: 0x3301", "address: 0x3301\n    data_type: uint8"
    ),
    "two_divergence_sensors": lambda s: s.replace(
        "    address: 0x3301\n",
        '    address: 0x3301\n    divergence:\n      name: "Autre"\n',
    ),
    "divergence_without_m2": lambda s: _drop_first(
        s,
        "  - platform: visslink\n"
        "    visslink_id: viessmann\n"
        '    name: "Plancher chauffant - Mode actuel"\n'
        "    address: 0x3301\n"
        "    update_interval: 5min\n",
    ),
}


_M2_SELECT = (
    "  - platform: visslink\n"
    "    visslink_id: viessmann\n"
    '    name: "Plancher chauffant - Mode actuel"\n'
    "    address: 0x3301\n"
    "    update_interval: 5min\n"
)


def _m2_as_text(text, type_="operating_mode", extra=""):
    """M2 déclaré en text_sensor (lecture seule) au lieu du select."""
    return _drop_first(text, _M2_SELECT) + (
        f"\n  - platform: visslink\n    type: {type_}\n"
        f'    name: "M2"\n    address: 0x3301\n{extra}'
    )


def _drop_first(text, fragment):
    assert fragment in text, fragment
    return text.replace(fragment, "", 1)


@pytest.mark.parametrize("case", sorted(INVALID_CASES))
def test_invalid_config_is_rejected(repo_root, tmp_path, case):
    result = _run_config(repo_root, tmp_path, INVALID_CASES[case])
    assert result.returncode != 0, f"{case} should be rejected:\n{result.stdout}"


# --- Compatibilité v0.1 --------------------------------------------------


def test_v01_config_still_valid_and_defaults_to_int16(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path, example=EXAMPLE_V01)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert "data_type" not in (repo_root / EXAMPLE_V01).read_text(encoding="utf-8")
    # Les trois capteurs de la v0.1 reçoivent le type par défaut.
    assert out.count("data_type: int16") == 3, out


def test_v01_fixture_matches_released_example(repo_root):
    text = (repo_root / EXAMPLE_V01).read_text(encoding="utf-8")
    assert text.startswith("# VissLink v0.1")


# --- Compatibilité v0.2 --------------------------------------------------


def test_v02_config_still_valid(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path, example=EXAMPLE_V02)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    text = (repo_root / EXAMPLE_V02).read_text(encoding="utf-8")
    assert text.startswith("# VissLink v0.2 ")
    assert "text_sensor:" not in text


# --- Programmations horaires (v0.2.1) ----------------------------------------

SCHEDULES = (0x2000, 0x3000, 0x2100, 0x2200)


def test_example_declares_four_schedules(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    shown = {int(a, 16) for a in re.findall(r"address: (0x[0-9a-fA-F]+)", out)}
    for address in SCHEDULES:
        assert address in shown, hex(address)
    assert out.count("update_interval: 6h") == 4, out


def _minimal_schedule(text):
    """Remplace les programmations de l'exemple par une seule, minimale."""
    head, _, _ = text.partition("\ntext_sensor:\n")
    return (
        head
        + "\ntext_sensor:\n"
        + "  - platform: visslink\n"
        + '    name: "Radiateurs - Programmation"\n'
        + "    address: 0x2000\n"
    )


def test_minimal_schedule_defaults_to_6h_without_clock(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path, _minimal_schedule)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert "update_interval: 6h" in out, out


def test_schedule_day_sensors_are_accepted(repo_root, tmp_path):
    days = (
        "monday",
        "tuesday",
        "wednesday",
        "thursday",
        "friday",
        "saturday",
        "sunday",
    )

    def transform(text):
        text = _minimal_schedule(text)
        return text + "".join(
            f'    {day}:\n      name: "Radiateurs - {day}"\n' for day in days
        )

    result = _run_config(repo_root, tmp_path, transform)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    for day in days:
        assert f"Radiateurs - {day}" in out, day


def test_schedule_last_valid_address(repo_root, tmp_path):
    # 0xFFC8 + 6 x 8 + 7 = 0xFFFF : le dimanche tient encore sur 16 bits.
    result = _run_config(
        repo_root,
        tmp_path,
        lambda s: s.replace("address: 0x2200", "address: 0xFFC8"),
    )
    assert result.returncode == 0, result.stdout + result.stderr


def test_schedule_next_to_another_register_is_accepted(repo_root, tmp_path):
    # 0x2038 suit immédiatement le dimanche de 0x2000 (0x2030-0x2037).
    result = _run_config(
        repo_root,
        tmp_path,
        lambda s: s.replace("address: 0x0804", "address: 0x2038"),
    )
    assert result.returncode == 0, result.stdout + result.stderr


# --- Types de données ------------------------------------------------------


@pytest.mark.parametrize("data_type", ["uint8", "int16", "uint16", "uint32"])
def test_each_data_type_is_accepted(repo_root, tmp_path, data_type):
    result = _run_config(
        repo_root,
        tmp_path,
        lambda s: s.replace("data_type: int16", f"data_type: {data_type}", 1),
    )
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert f"data_type: {data_type}" in out


def test_data_type_is_case_insensitive(repo_root, tmp_path):
    result = _run_config(
        repo_root,
        tmp_path,
        lambda s: s.replace("data_type: uint32", "data_type: UINT32", 1),
    )
    assert result.returncode == 0, result.stdout + result.stderr


# --- Registres expérimentaux ----------------------------------------------

_COMMENTED = re.compile(r"^  # (- |  )", re.MULTILINE)
_RAW_0846 = re.compile(
    r'  # - platform: visslink\n  #   name: "Brut 0x0846[^\n]*\n(  #   [^\n]*\n)*'
)


def _enable_experimental(text):
    """Décommente les entités des sections sensor/binary_sensor uniquement."""
    head, sep, tail = text.partition("\nsensor:\n")
    return head + sep + _COMMENTED.sub(r"  \1", tail)


def test_example_keeps_experimental_registers_commented(repo_root):
    text = (repo_root / EXAMPLE).read_text(encoding="utf-8")
    active = [line for line in text.splitlines() if not line.lstrip().startswith("#")]
    active = "\n".join(active)
    for address in ("0x5502", "0x0846", "0x0845", "0x0883", "0x2906", "0x3906"):
        assert f"address: {address}" not in active, address
        assert f"address: {address}" in text, address


def test_experimental_registers_are_valid_once_enabled(repo_root, tmp_path):
    def transform(text):
        text = _RAW_0846.sub("", text)  # étape 1 terminée : entrée brute retirée
        return _enable_experimental(text)

    assert "Brut 0x0846" not in transform(
        (repo_root / EXAMPLE).read_text(encoding="utf-8")
    )
    result = _run_config(repo_root, tmp_path, transform)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    # `esphome config` affiche les adresses sans zéro de tête (0x846).
    shown = {int(a, 16) for a in re.findall(r"address: (0x[0-9a-fA-F]+)", out)}
    for address in (0x5502, 0x0846, 0x0845, 0x0883, 0x2906, 0x3906):
        assert address in shown, hex(address)


def test_raw_check_and_binary_on_same_register_is_rejected(repo_root, tmp_path):
    # Vérification brute (uint8) et binary_sensor actifs ensemble : doublon.
    result = _run_config(repo_root, tmp_path, _enable_experimental)
    out = result.stdout + result.stderr
    assert result.returncode != 0, out
    assert "0x0846" in out, out


# --- Configuration de diagnostic (v0.2.2) -------------------------------------

EXPERIMENTAL = {
    0x0846: "uint8",
    0x0845: "uint8",
    0x2906: "uint8",
    0x3906: "uint8",
    0x0883: "uint8",
    0x5502: "int16",
}
_TEST_ENTRY = re.compile(
    r'  - platform: visslink\n    name: "TEST [^\n]*\n(    [^\n]*\n)*'
)


def _active_lines(text):
    lines = (line for line in text.splitlines() if line.strip())
    return "\n".join(line for line in lines if not line.lstrip().startswith("#"))


def test_diagnostic_config_reads_experimental_registers_raw(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path, example=DIAGNOSTIC)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    entries = re.findall(
        r"name: TEST [^\n]*\n((?:    [^\n]*\n)*)", out
    )  # sortie de `esphome config` : une entrée par capteur TEST
    assert len(entries) == len(EXPERIMENTAL), out
    seen = {}
    for entry in entries:
        address = int(re.search(r"address: (0x[0-9a-fA-F]+)", entry).group(1), 16)
        seen[address] = re.search(r"data_type: (\w+)", entry).group(1)
        assert "entity_category: diagnostic" in entry, entry
        assert "filters" not in entry and "unit_of_measurement" not in entry, entry
    assert seen == EXPERIMENTAL


def test_diagnostic_config_is_the_example_plus_test_sensors(repo_root):
    # Même nœud, mêmes Wi-Fi / API / OTA / UART / registres que l'exemple
    # v0.2.2 : seules les six entrées TEST (et les commentaires) diffèrent.
    example = (repo_root / EXAMPLE_V022).read_text(encoding="utf-8")
    diagnostic = (repo_root / DIAGNOSTIC).read_text(encoding="utf-8")
    assert len(_TEST_ENTRY.findall(diagnostic)) == len(EXPERIMENTAL)
    assert _active_lines(_TEST_ENTRY.sub("", diagnostic)) == _active_lines(example)


# --- Écritures et consignes (v0.3) --------------------------------------------

WRITABLE = {
    0x2306: (3, 37),
    0x2307: (3, 37),
    0x3306: (3, 37),
    0x3307: (3, 37),
    0x6300: (10, 60),
}


def test_example_writes_enabled_by_default(repo_root, tmp_path):
    """Aucune autorisation déclarée : les six sont actives, le bouclage les
    sept jours ; les avertissements le rappellent à la compilation."""
    text = _active_lines((repo_root / EXAMPLE).read_text(encoding="utf-8"))
    assert "enable_" not in text
    assert "validated_days" not in text
    result = _run_config(repo_root, tmp_path)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    for flag in WRITE_FLAGS:
        assert f"{flag}: true" in out, flag
    for day in SEVEN_DAYS:
        assert f"  - {day}\n" in out, day
    assert "Optolink WRITES ENABLED" in out, out


def test_enable_writes_false_disables_everything(repo_root, tmp_path):
    """Compatibilité : enable_writes: false seul désactive les six
    autorisations, sans erreur ni avertissement."""
    result = _run_config(repo_root, tmp_path, _with_flags(enable_writes="false"))
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    for flag in WRITE_FLAGS:
        assert f"{flag}: false" in out, flag
    assert "ENABLED" not in out


@pytest.mark.parametrize("flag", WRITE_FLAGS[1:])
def test_each_flag_false_disables_only_its_feature(repo_root, tmp_path, flag):
    result = _run_config(repo_root, tmp_path, _with_flags(**{flag: "false"}))
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert f"{flag}: false" in out, out
    # Le programme de M2 suit celui des programmes ; rien d'autre ne change.
    off = {flag} | ({"enable_m2_mode_write"} if flag == "enable_mode_writes" else set())
    for other in WRITE_FLAGS:
        assert f"{other}: {'false' if other in off else 'true'}" in out, other


def test_example_setpoints_are_numbers(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    _, _, numbers = out.partition("\nnumber:\n")
    numbers = numbers.partition("\nselect:\n")[0].partition("\ntext_sensor:\n")[0]
    shown = {
        int(a, 16): (int(lo), int(hi))
        for a, lo, hi in re.findall(
            r"address: (0x[0-9a-fA-F]+)\n    min_value: (\d+)\n    max_value: (\d+)",
            numbers,
        )
    }
    assert set(shown) == set(WRITABLE), numbers
    for address, (lo, hi) in shown.items():
        assert WRITABLE[address][0] <= lo < hi <= WRITABLE[address][1]
    assert numbers.count("update_interval: 5min") == 5, numbers
    assert numbers.count("step: 1") == 5, numbers
    # Les consignes ne sont plus déclarées en capteurs (adresse unique).
    sensors = _active_lines((repo_root / EXAMPLE).read_text(encoding="utf-8"))
    sensors = sensors.partition("\nsensor:")[2].partition("\nbinary_sensor:")[0]
    for address in WRITABLE:
        assert f"address: 0x{address:04X}" not in sensors


def test_target_number_syntax_is_accepted(repo_root, tmp_path):
    # API cible de la v0.3, telle que spécifiée.
    def transform(text):
        head, _, _ = text.partition("\nnumber:\n")
        return head + (
            "\nnumber:\n"
            "  - platform: visslink\n"
            "    visslink_id: viessmann\n"
            '    name: "Radiateurs - Consigne confort"\n'
            "    address: 0x2306\n"
            "    min_value: 16\n"
            "    max_value: 24\n"
            "    step: 1\n"
            '    unit_of_measurement: "°C"\n'
            "    mode: box\n"
        )

    result = _run_config(repo_root, tmp_path, transform)
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.mark.parametrize("address", sorted(WRITABLE))
def test_documented_bounds_are_accepted(repo_root, tmp_path, address):
    lo, hi = WRITABLE[address]

    def transform(text):
        head, _, _ = text.partition("\nnumber:\n")
        return head + (
            "\nnumber:\n"
            "  - platform: visslink\n"
            '    name: "Consigne"\n'
            f"    address: 0x{address:04X}\n"
            f"    min_value: {lo}\n"
            f"    max_value: {hi}\n"
        )

    result = _run_config(repo_root, tmp_path, transform)
    assert result.returncode == 0, result.stdout + result.stderr


def test_python_and_cpp_whitelists_match(repo_root):
    cpp = (repo_root / "components/visslink/write_guard.h").read_text(encoding="utf-8")
    cpp_table = {
        int(a, 16): (int(lo), int(hi))
        for a, lo, hi in re.findall(r"\{0x([0-9A-Fa-f]{4}), 1, (\d+), (\d+)\}", cpp)
    }
    py = (repo_root / "components/visslink/number.py").read_text(encoding="utf-8")
    block = py.partition("WRITABLE_REGISTERS = {")[2].partition("}")[0]
    py_table = {
        int(a, 16): (int(lo), int(hi))
        for a, lo, hi in re.findall(r"0x([0-9A-Fa-f]{4}): \((\d+), (\d+)\)", block)
    }
    assert cpp_table == WRITABLE
    assert py_table == WRITABLE


# --- Compatibilité v0.2.1 / v0.2.2 ---------------------------------------------


@pytest.mark.parametrize("fixture", [EXAMPLE_V022, DIAGNOSTIC_V022])
def test_v022_configs_still_valid(repo_root, tmp_path, fixture):
    result = _run_config(repo_root, tmp_path, example=fixture)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    # Sans `enable_writes` : autorisé par défaut, mais la v0.2.2 n'a aucune
    # entité inscriptible (ni number ni select).
    assert "enable_writes: true" in out, out
    assert "\nnumber:" not in (repo_root / fixture).read_text(encoding="utf-8")


def test_cpp_writes_disabled_by_default_and_never_restored(repo_root):
    # Côté C++, sans appel à set_enable_writes(), aucune écriture : le défaut
    # du membre est false. La plateforme number ne restaure aucune valeur.
    header = (repo_root / "components/visslink/visslink.h").read_text(encoding="utf-8")
    assert "bool enable_writes_{false};" in header
    assert "write_allowed(uint8_t /*value*/) const { return false; }" in header
    sources = header + (repo_root / "components/visslink/visslink.cpp").read_text(
        encoding="utf-8"
    )
    for forbidden in ("restore_value", "rtc_", "global_preferences", "load(&"):
        assert forbidden not in sources, forbidden
    number_py = (repo_root / "components/visslink/number.py").read_text(
        encoding="utf-8"
    )
    assert "CONF_RESTORE_VALUE" not in number_py
    assert "CONF_INITIAL_VALUE" not in number_py


# --- Programmes de fonctionnement (v0.3, expérimental) -----------------------

MODES = {0x00: "ECS seule", 0x03: "Chauffage + ECS", 0x05: "Arrêt"}


def test_explicit_true_flags_still_accepted(repo_root, tmp_path):
    """Compatibilité : les configurations qui déclarent les six autorisations
    à true restent valides."""
    result = _run_config(
        repo_root, tmp_path, _with_flags(**{flag: "true" for flag in WRITE_FLAGS})
    )
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    for flag in WRITE_FLAGS:
        assert f"{flag}: true" in out, flag


def test_mode_writes_need_both_flags_and_warn(repo_root, tmp_path):
    result = _run_config(
        repo_root,
        tmp_path,
        lambda s: s.replace("enable_writes: false", "enable_writes: true").replace(
            "enable_mode_writes: false", "enable_mode_writes: true"
        ),
    )
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert "OPERATING MODE WRITES ENABLED" in out, out


def test_example_mode_entities(repo_root, tmp_path):
    # Une entité par circuit, deux select, sans doublon numérique.
    result = _run_config(repo_root, tmp_path)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    selects = out.partition("\nselect:\n")[2].partition("\ntext_sensor:\n")[0]
    shown = re.findall(r"name: ([^\n]+)\n    address: (0x[0-9a-fA-F]+)", selects)
    assert shown == [
        ("Radiateurs - Mode actuel", "0x2301"),
        ("Plancher chauffant - Mode actuel", "0x3301"),
    ], selects
    assert "type: operating_mode" not in out
    assert out.count("divergence:") == 1, out
    for address in ("0x2301", "0x3301"):
        assert out.count(f"address: {address}\n") == 1, address  # aucun doublon


def test_explicit_false_flags_still_accepted(repo_root, tmp_path):
    """Compatibilité : une configuration qui déclare les six autorisations à
    false (exemples v0.3 à v0.7.1) reste valide et en lecture seule."""
    result = _run_config(
        repo_root, tmp_path, _with_flags(**{flag: "false" for flag in WRITE_FLAGS})
    )
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    for flag in WRITE_FLAGS:
        assert f"{flag}: false" in out, flag
    assert "ENABLED" not in out


def test_all_three_write_flags_valid_with_warnings(repo_root, tmp_path):
    def transform(s):
        for flag in ("enable_writes", "enable_mode_writes", "enable_m2_mode_write"):
            s = s.replace(f"{flag}: false", f"{flag}: true")
        return s

    result = _run_config(repo_root, tmp_path, transform)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert "M2 OPERATING MODE WRITE ENABLED" in out, out
    assert "OPERATING MODE WRITES ENABLED" in out, out


def test_mode_text_sensors_still_valid(repo_root, tmp_path):
    # Lecture seule stricte possible pour les deux circuits (comme en v0.3).
    def transform(text):
        head, _, tail = text.partition("\nselect:\n")
        tail = tail.partition("\n# --- Programmations horaires")[2]
        return (
            head
            + "\n# --- Programmations horaires"
            + tail
            + "\n  - platform: visslink\n    type: operating_mode\n"
            + '    name: "A1"\n    address: 0x2301\n'
            + '    divergence:\n      name: "Div"\n'
            + "\n  - platform: visslink\n    type: operating_mode\n"
            + '    name: "M2"\n    address: 0x3301\n'
        )

    result = _run_config(repo_root, tmp_path, transform)
    assert result.returncode == 0, result.stdout + result.stderr


def test_mode_tables_match_cpp(repo_root):
    cpp = (repo_root / "components/visslink/write_guard.h").read_text(encoding="utf-8")
    values = re.search(r"OPERATING_MODE_VALUES\[\] = \{([^}]*)\}", cpp).group(1)
    readable = re.search(r"MODE_REGISTERS\[\] = \{([^}]*)\}", cpp).group(1)
    writable = re.search(r"MODE_WRITE_WHITELIST\[\] = \{([^}]*)\}", cpp).group(1)

    def as_ints(text):
        return [int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]+", text)]

    assert as_ints(values) == list(MODES)
    assert as_ints(readable) == [0x2301, 0x3301]
    assert as_ints(writable) == [0x2301, 0x3301]  # M2 : troisième autorisation
    assert "M2_MODE_REGISTER = 0x3301;" in cpp
    texts = re.findall(r'case 0x0([035]):\n\s+return "([^"]+)";', cpp)
    assert [(int(v), t) for v, t in texts] == list(MODES.items())
    assert 'return "Inconnu";' in cpp
    py = (repo_root / "components/visslink/__init__.py").read_text(encoding="utf-8")
    assert "MODE_REGISTERS = (0x2301, 0x3301)" in py
    assert "MODE_WRITE_WHITELIST = (0x2301, 0x3301)" in py
    assert "M2_MODE_REGISTER = 0x3301" in py
    labels = re.findall(r'0x0([035]): "([^"]+)"', py)
    assert [(int(v), label) for v, label in labels] == list(MODES.items())
    for forbidden in ("Party", "Congé", "Ferien"):
        assert f'"{forbidden}"' not in py


def test_v03_config_still_valid(repo_root, tmp_path):
    # Exemple v0.3 inchangé (M2 en text_sensor) : valide, M2 non inscriptible.
    result = _run_config(repo_root, tmp_path, example=EXAMPLE_V03)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    text = (repo_root / EXAMPLE_V03).read_text(encoding="utf-8")
    assert text.startswith("# VissLink v0.3 ")
    assert "enable_m2_mode_write" not in text
    assert "enable_m2_mode_write: false" in out, out
    assert "type: operating_mode" in out


def test_m2_as_text_sensor_is_valid(repo_root, tmp_path):
    # Témoin des deux cas invalides ci-dessus : sans l'erreur injectée, la
    # même transformation est valide.
    result = _run_config(repo_root, tmp_path, _m2_as_text)
    assert result.returncode == 0, result.stdout + result.stderr


def test_v04_config_still_valid(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path, example=EXAMPLE_V04)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    text = (repo_root / EXAMPLE_V04).read_text(encoding="utf-8")
    assert text.startswith("# VissLink v0.4 ")
    for flag in ("enable_writes", "enable_mode_writes", "enable_m2_mode_write"):
        assert f"{flag}: false" in out, flag


# --- Diagnostic bouclage ECS (lecture seule) ----------------------------------

DIAGNOSTIC_LOOP = "examples/iot-viessmann-diagnostic-bouclage.yaml"


def test_loop_diagnostic_valid(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path, example=DIAGNOSTIC_LOOP)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    for flag in ("enable_writes", "enable_mode_writes", "enable_m2_mode_write"):
        assert f"{flag}: true" in out, flag  # défaut
    # 0x0846 lu en octet brut, programmation du bouclage relue toutes les 10 min.
    assert re.search(
        r"name: TEST — Bouclage ECS brut\n(?:    [^\n]*\n)*?    address: 0x846\n", out
    ), out
    loop = out.partition("name: Bouclage ECS - Programmation\n")[2]
    loop = loop.partition("\n  - platform:")[0]
    assert "update_interval: 10min" in loop, loop
    for day in (
        "lundi",
        "mardi",
        "mercredi",
        "jeudi",
        "vendredi",
        "samedi",
        "dimanche",
    ):
        assert f"TEST — Bouclage {day}" in loop, day


def test_loop_diagnostic_only_adds_read_entities(repo_root):
    # Mêmes nœud, Wi-Fi, API, OTA, UART, autorisations et entités que
    # l'exemple v0.4.1 dont il est issu : seuls s'ajoutent le capteur 0x0846,
    # le détail des 7 jours et l'intervalle de 10 min du bouclage.
    example = _active_lines((repo_root / EXAMPLE_V041).read_text(encoding="utf-8"))
    diag = _active_lines((repo_root / DIAGNOSTIC_LOOP).read_text(encoding="utf-8"))
    removed = [line for line in example.splitlines() if line not in diag.splitlines()]
    # Autorisations à false de la v0.4.1 : retirées (défaut true).
    assert removed == [
        "  enable_writes: false",
        "  enable_mode_writes: false",
        "  enable_m2_mode_write: false",
    ], removed
    added = [line for line in diag.splitlines() if line not in example.splitlines()]
    allowed = re.compile(
        r"^\s+(- platform: visslink|name: \"TEST — Bouclage[^\"]*\"|address: 0x0846|"
        r"data_type: uint8|update_interval: (60s|10min)|accuracy_decimals: 0|"
        r"entity_category: diagnostic|(monday|tuesday|wednesday|thursday|friday|"
        r"saturday|sunday):)$"
    )
    assert all(allowed.match(line) for line in added), added


# --- v0.5 : bouclage ECS à la demande -----------------------------------------


def test_v041_config_still_valid(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path, example=EXAMPLE_V041)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert (
        (repo_root / EXAMPLE_V041)
        .read_text(encoding="utf-8")
        .startswith("# VissLink v0.4.1 ")
    )
    assert "enable_schedule_writes: false" in out, out


def test_example_boost_present_all_days_by_default(repo_root, tmp_path):
    text = _active_lines((repo_root / EXAMPLE).read_text(encoding="utf-8"))
    assert "enable_schedule_writes" not in text
    assert "validated_days" not in text
    result = _run_config(repo_root, tmp_path)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert "visslink_boost:" in out
    assert "  validated_days:\n" + "".join(f"    - {d}\n" for d in SEVEN_DAYS) in out, (
        out
    )
    assert "SCHEDULE BLOCK WRITES ENABLED" in out
    # Horloge lue en lecture seule, à 0x088E.
    assert re.search(
        r"name: Vitotronic - Horloge\n(?:    [^\n]*\n)*?    address: 0x88E\n", out
    ), out


def test_boost_days_can_still_be_restricted(repo_root, tmp_path):
    for days, expected in (("[]", "validated_days: []"), ("[sunday]", "  - sunday\n")):
        result = _run_config(
            repo_root,
            tmp_path,
            lambda s, d=days: s.replace(
                "\n  time_id: ha_time\n",
                f"\n  time_id: ha_time\n  validated_days: {d}\n",
                1,
            ),
        )
        out = result.stdout + result.stderr
        assert result.returncode == 0, out
        assert expected in out, out
        assert "  - monday\n" not in out.partition("visslink_boost:")[2], out


def test_schedule_writes_need_enable_writes(repo_root, tmp_path):
    bad = _run_config(
        repo_root,
        tmp_path,
        _with_flags(enable_writes="false", enable_schedule_writes="true"),
    )
    assert bad.returncode != 0, bad.stdout
    off = _run_config(repo_root, tmp_path, _with_flags(enable_schedule_writes="false"))
    out = off.stdout + off.stderr
    assert off.returncode == 0, out
    assert "enable_schedule_writes: false" in out, out
    assert "SCHEDULE BLOCK WRITES ENABLED" not in out


@pytest.mark.parametrize(
    "case",
    [
        lambda s: s.replace(
            "\n  time_id: ha_time\n",
            "\n  time_id: ha_time\n  validated_days: [dimanche]\n",
            1,
        ),
        lambda s: s.replace("  time_id: ha_time\n  start:", "  start:"),
        lambda s: _drop_first(
            s, '  start:\n    name: "Bouclage ECS — Démarrer maintenant"\n'
        ),
        lambda s: s.replace(
            "type: vito_clock\n", "type: vito_clock\n    address: 0x0846\n"
        ),
        lambda s: s.replace(
            '  start:\n    name: "Bouclage ECS — Démarrer maintenant"\n',
            '  start:\n    name: "Bouclage ECS — Démarrer maintenant"\n  address: 0x0846\n',
        ),
    ],
)
def test_boost_invalid_configs(repo_root, tmp_path, case):
    result = _run_config(repo_root, tmp_path, case)
    assert result.returncode != 0, result.stdout


def test_boost_validated_days_accepted(repo_root, tmp_path):
    result = _run_config(
        repo_root,
        tmp_path,
        lambda s: s.replace(
            "\n  time_id: ha_time\n",
            "\n  time_id: ha_time\n  validated_days: [sunday, Monday]\n",
            1,
        ),
    )
    out = result.stdout + result.stderr
    assert result.returncode == 0, out


def test_boost_tables_match_cpp(repo_root):
    cpp = (repo_root / "components/visslink/circulation_boost.h").read_text(
        encoding="utf-8"
    )
    assert "LOOP_SCHEDULE_MONDAY = 0x2200;" in cpp
    durations = re.search(r"BOOST_DURATIONS\[\] = \{([^}]*)\}", cpp).group(1)
    py = (repo_root / "components/visslink_boost/__init__.py").read_text(
        encoding="utf-8"
    )
    options = re.search(r"DURATION_OPTIONS = \[([^\]]*)\]", py).group(1)
    assert [int(d) for d in re.findall(r"\d+", durations)] == [
        int(d) for d in re.findall(r"\d+", options)
    ]
    # Jamais d'écriture sur 0x0846 : aucune occurrence dans le code d'écriture
    # (hors commentaires).
    for f in ("circulation_boost.h", "write_guard.h"):
        text = (repo_root / "components/visslink" / f).read_text(encoding="utf-8")
        code = re.sub(r"//[^\n]*", "", text)
        assert "0x0846" not in code and "0x846" not in code, f


def test_loop_diagnostic_still_valid_with_v05(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path, example=DIAGNOSTIC_LOOP)
    assert result.returncode == 0, result.stdout + result.stderr


# --- v0.5 : états Éco et Party (lecture seule) --------------------------------

ECO_PARTY = {
    0x2302: "Radiateurs - Éco",
    0x2303: "Radiateurs - Party",
    0x3302: "Plancher chauffant - Éco",
    0x3303: "Plancher chauffant - Party",
}


def test_example_eco_party_binary_sensors(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    binaries = out.partition("\nbinary_sensor:\n")[2].partition("\nnumber:\n")[0]
    for address, name in ECO_PARTY.items():
        entry = re.search(rf"name: {re.escape(name)}\n((?:    [^\n]*\n)*)", binaries)
        assert entry, name
        assert f"address: 0x{address:X}\n" in entry.group(1), name
        assert "update_interval: 60s" in entry.group(1), name  # v0.5.1 (30 s en v0.5)
        # Une seule déclaration de chaque adresse, toutes plateformes confondues.
        assert out.count(f"address: 0x{address:X}\n") == 1, hex(address)


@pytest.mark.parametrize("address", sorted(ECO_PARTY))
def test_eco_party_never_writable(repo_root, tmp_path, address):
    # Ni consigne (number) ni programme (select) sur ces adresses : refusé par
    # la liste blanche, pas seulement comme doublon.
    for old, expected in (
        ("address: 0x2306", "not in the VissLink write whitelist"),
        ("address: 0x2301", "is not an operating mode register"),
    ):
        result = _run_config(
            repo_root,
            tmp_path,
            lambda s, old=old: s.replace(old, f"address: 0x{address:04X}"),
        )
        out = result.stdout + result.stderr
        assert result.returncode != 0, hex(address)
        assert expected in out, out


def test_eco_party_duplicate_rejected(repo_root, tmp_path):
    result = _run_config(
        repo_root, tmp_path, lambda s: s.replace("address: 0x3303", "address: 0x3302")
    )
    assert result.returncode != 0, result.stdout


# --- Essai en lecture seule v0.5 ----------------------------------------------

ESSAI_V05 = "examples/iot-viessmann-essai-lecture-v0.5.yaml"


def test_read_trial_config_valid(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path, example=ESSAI_V05)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    for address in ("0x88E", "0x846", "0x2302", "0x2303", "0x3302", "0x3303"):
        assert out.count(f"address: {address}\n") == 1, address


def test_read_only_trial_only_adds_read_entities(repo_root):
    example = _active_lines((repo_root / EXAMPLE).read_text(encoding="utf-8"))
    trial = _active_lines((repo_root / ESSAI_V05).read_text(encoding="utf-8"))
    removed = [line for line in example.splitlines() if line not in trial.splitlines()]
    assert removed == [], removed
    added = [line for line in trial.splitlines() if line not in example.splitlines()]
    allowed = re.compile(
        r"^\s+(- platform: visslink|visslink_id: viessmann|"
        r"name: \"TEST — Bouclage[^\"]*\"|address: 0x0846|"
        r"data_type: uint8|update_interval: (60s|10min)|accuracy_decimals: 0|"
        r"entity_category: diagnostic|(monday|tuesday|wednesday|thursday|friday|"
        r"saturday|sunday):)$"
    )
    assert all(allowed.match(line) for line in added), added


# --- v0.5.1 : valeurs « non disponibles » (invalid_values) ------------------


def _flow_setpoint_sensor(extra, data_type="int16", address="0x2544"):
    def transform(text):
        sensor = (
            "sensor:\n"
            "  - platform: visslink\n"
            '    name: "TEST consigne départ"\n'
            f"    address: {address}\n"
            f"    data_type: {data_type}\n"
            f"{extra}"
            "    filters:\n"
            "      - multiply: 0.1\n"
        )
        assert text.count("\nsensor:\n") == 1
        return text.replace("\nsensor:\n", "\n" + sensor, 1)

    return transform


@pytest.mark.parametrize(
    "extra, data_type",
    [
        ("    invalid_values: 0xFFFF\n", "int16"),
        ("    invalid_values: [0xFFFF, 0x7FFF]\n", "int16"),
        ("    invalid_values: [0xFF]\n", "uint8"),
        ("    invalid_values: [0xFFFFFFFF]\n", "uint32"),
    ],
)
def test_invalid_values_accepted(repo_root, tmp_path, extra, data_type):
    result = _run_config(repo_root, tmp_path, _flow_setpoint_sensor(extra, data_type))
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert "invalid_values" in out


@pytest.mark.parametrize(
    "extra, data_type, message",
    [
        ("    invalid_values: [0x10000]\n", "int16", "does not fit in int16"),
        ("    invalid_values: [0x100]\n", "uint8", "does not fit in uint8"),
        ("    invalid_values: [0xFFFF, 0xFFFF]\n", "int16", "duplicate value"),
        ("    invalid_values: [1, 2, 3, 4, 5]\n", "int16", "at most 4"),
        ("    invalid_values: []\n", "int16", "at least 1"),
    ],
)
def test_invalid_values_rejected(repo_root, tmp_path, extra, data_type, message):
    result = _run_config(repo_root, tmp_path, _flow_setpoint_sensor(extra, data_type))
    out = result.stdout + result.stderr
    assert result.returncode != 0, out
    assert message in out, out


def test_invalid_values_opt_in_only(repo_root):
    """Aucune valeur n'est invalide par défaut : FF FF reste -0,1 °C sur la
    température extérieure (0x0800), et l'option n'existe que pour `sensor`."""
    sensor_py = (repo_root / "components/visslink/sensor.py").read_text(
        encoding="utf-8"
    )
    assert "cv.Optional(CONF_INVALID_VALUES)" in sensor_py
    assert "CONF_INVALID_VALUES, default" not in sensor_py
    for name in ("binary_sensor.py", "number.py", "select.py", "text_sensor.py"):
        text = (repo_root / "components/visslink" / name).read_text(encoding="utf-8")
        assert "invalid_values" not in text


# --- v0.5.1 : décisions de configuration --------------------------------------

_ECO_PARTY = ("0x2302", "0x2303", "0x3302", "0x3303")


def _interval_after(text, address):
    match = re.search(
        rf"^    address: {address}\n    update_interval: (\S+)$", text, re.MULTILINE
    )
    assert match, address
    return match.group(1)


@pytest.mark.parametrize("example", [EXAMPLE, ESSAI_V05])
def test_eco_party_read_every_60s(repo_root, example):
    text = (repo_root / example).read_text(encoding="utf-8")
    for address in _ECO_PARTY:
        assert _interval_after(text, address) == "60s", address


@pytest.mark.parametrize("example", [EXAMPLE, ESSAI_V05])
def test_flow_setpoints_commented_with_invalid_values(repo_root, example):
    text = (repo_root / example).read_text(encoding="utf-8")
    for address in ("0x2544", "0x3544"):
        block = re.search(
            rf"  # - platform: visslink\n(  #   [^\n]*\n)*?  #   address: {address}\n"
            rf"(  #   [^\n]*\n)*?",
            text,
        )
        assert block, address
        start = block.start()
        following = text[start : start + 400]
        assert f"  #   address: {address}\n" in following
        assert "  #   invalid_values: [0xFFFF]\n" in following
        # Jamais actif par défaut.
        assert f"    address: {address}\n" not in _active_lines(text)


def test_no_write_path_for_loop_pump_0x0846(repo_root):
    """0x0846 n'est inscriptible par aucun chemin : listes blanches C++ et
    Python (consignes, programmes), blocs horaires du bouclage (0x2200..0x2237
    seulement, testé en C++). Seule mention permise : une phrase de
    documentation rappelant qu'il n'est jamais commandé."""
    root = repo_root / "components"
    for path in sorted(root.rglob("*")):
        if path.suffix not in (".py", ".h", ".cpp") or "__pycache__" in path.parts:
            continue
        for line in path.read_text(encoding="utf-8").splitlines():
            if "0846" in line:
                assert "jamais" in line, f"{path}: {line}"


# --- v0.5.2 : configuration d'essai matériel du bouclage --------------------

ESSAI_BOUCLAGE = "examples/iot-viessmann-essai-bouclage-v0.5.2.yaml"


def test_loop_hardware_trial_valid_and_without_writes(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path, example=ESSAI_BOUCLAGE)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    # 0x0846 déclaré une seule fois, en octet brut lu toutes les 15 s.
    assert out.count("address: 0x846\n") == 1, out
    assert re.search(
        r"address: 0x846\n(?:    [^\n]*\n)*?    update_interval: 15s\n", out
    ), out
    assert "name: Bouclage ECS — Restauration en cours" in out, out


def test_loop_hardware_trial_only_adds_read_entities(repo_root):
    example = _active_lines((repo_root / EXAMPLE).read_text(encoding="utf-8"))
    trial = _active_lines((repo_root / ESSAI_BOUCLAGE).read_text(encoding="utf-8"))
    removed = [line for line in example.splitlines() if line not in trial.splitlines()]
    assert removed == [], removed
    added = [line for line in trial.splitlines() if line not in example.splitlines()]
    allowed = re.compile(
        r"^\s+(- platform: visslink|visslink_id: viessmann|"
        r"name: \"TEST — Bouclage ECS brut\"|address: 0x0846|"
        r"data_type: uint8|update_interval: 15s|accuracy_decimals: 0|"
        r"entity_category: diagnostic|restoring:|"
        r"name: \"Bouclage ECS — Restauration en cours\")$"
    )
    assert all(allowed.match(line) for line in added), added


def test_production_keeps_loop_pump_at_60s(repo_root):
    """Production : 0x0846 seulement en commentaire, à 60 s ; 15 s est
    réservé à l'essai matériel."""
    text = (repo_root / EXAMPLE).read_text(encoding="utf-8")
    assert "0x0846" not in _active_lines(text)
    intervals = re.findall(
        r"address: 0x0846\n(?:  #   (?!- platform)[^\n]*\n)*?  #   update_interval: (\S+)",
        text,
    )
    assert intervals and set(intervals) == {"60s"}, intervals


def test_loop_pump_0x0846_never_writable_in_examples(repo_root):
    """Aucune entité inscriptible (number, select) sur 0x0846 dans les
    exemples : seulement sensor / binary_sensor."""
    for path in sorted((repo_root / "examples").glob("*.yaml")):
        if path.name == "secrets.yaml":
            continue
        text = _active_lines(path.read_text(encoding="utf-8"))
        section = None
        for line in text.splitlines():
            if not line.startswith(" "):
                section = line.rstrip(":")
            if "address: 0x0846" in line:
                assert section in ("sensor", "binary_sensor"), (path, section)


def test_boost_default_duration_is_10_min(repo_root):
    """Durée proposée après chaque redémarrage de l'ESP : 10 min."""
    init = (repo_root / "components/visslink_boost/__init__.py").read_text(
        encoding="utf-8"
    )
    options = re.search(r"DURATION_OPTIONS = (\[[^\]]*\])", init).group(1)
    default = re.search(r'DEFAULT_DURATION = "([^"]*)"', init).group(1)
    assert default == "10 min"
    header = (repo_root / "components/visslink/circulation_boost.h").read_text(
        encoding="utf-8"
    )
    index = int(re.search(r"BOOST_DEFAULT_DURATION_INDEX = (\d+);", header).group(1))
    assert ast.literal_eval(options)[index] == default
    cpp = (repo_root / "components/visslink_boost/visslink_boost.cpp").read_text(
        encoding="utf-8"
    )
    assert "publish_state(visslink::BOOST_DEFAULT_DURATION_INDEX)" in cpp


def test_boost_restoring_entity_is_optional(repo_root, tmp_path):
    # La production n'a pas l'entité et reste valide (test_example_*) ; la
    # retirer de l'essai matériel le laisse valide aussi.
    def drop(text):
        old = '  restoring:\n    name: "Bouclage ECS — Restauration en cours"\n'
        assert old in text
        return text.replace(old, "")

    result = _run_config(repo_root, tmp_path, drop, example=ESSAI_BOUCLAGE)
    assert result.returncode == 0, result.stdout + result.stderr


# --- v0.6 : consigne d'eau chaude sanitaire ---------------------------------

ESSAI_CONSIGNE_ECS = "examples/iot-viessmann-essai-consigne-ecs-v0.6.yaml"


def test_dhw_setpoint_write_enabled_by_default_and_can_be_disabled(repo_root, tmp_path):
    text = _active_lines((repo_root / EXAMPLE).read_text(encoding="utf-8"))
    assert "enable_dhw_setpoint_write" not in text
    result = _run_config(repo_root, tmp_path)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert "enable_dhw_setpoint_write: true" in out, out
    assert "DHW SETPOINT WRITE ENABLED" in out
    off = _run_config(
        repo_root, tmp_path, _with_flags(enable_dhw_setpoint_write="false")
    )
    out = off.stdout + off.stderr
    assert off.returncode == 0, out
    assert "enable_dhw_setpoint_write: false" in out, out
    assert "DHW SETPOINT WRITE ENABLED" not in out


def test_dhw_setpoint_write_requires_enable_writes(repo_root, tmp_path):
    bad = _run_config(
        repo_root,
        tmp_path,
        _with_flags(enable_writes="false", enable_dhw_setpoint_write="true"),
    )
    assert bad.returncode != 0, bad.stdout
    assert "enable_dhw_setpoint_write requires enable_writes: true" in (
        bad.stdout + bad.stderr
    )
    # enable_writes: false seul : la consigne ECS suit, sans erreur.
    off = _run_config(repo_root, tmp_path, _with_flags(enable_writes="false"))
    out = off.stdout + off.stderr
    assert off.returncode == 0, out
    assert "enable_dhw_setpoint_write: false" in out, out


def test_dhw_limits_and_whitelist_match_cpp(repo_root):
    header = (repo_root / "components/visslink/write_guard.h").read_text(
        encoding="utf-8"
    )
    low = int(re.search(r"DHW_WRITE_MIN = (\d+);", header).group(1))
    high = int(re.search(r"DHW_WRITE_MAX = (\d+);", header).group(1))
    number_py = (repo_root / "components/visslink/number.py").read_text(
        encoding="utf-8"
    )
    limits = re.search(r"DHW_WRITE_LIMITS = \((\d+), (\d+)\)", number_py)
    assert (low, high) == (int(limits.group(1)), int(limits.group(2))) == (50, 60)
    assert "DHW_SETPOINT_ADDRESS = 0x6300" in number_py
    whitelist = re.search(
        r"DHW_SETPOINT_WRITE_WHITELIST\[\] = \{([^}]*)\}", header
    ).group(1)
    assert whitelist.strip() == "DHW_SETPOINT_ADDRESS"
    assert "static constexpr uint16_t DHW_SETPOINT_ADDRESS = 0x6300;" in header
    # Dans la plage documentée de la liste blanche générale.
    assert WRITABLE[0x6300][0] <= low and high <= WRITABLE[0x6300][1]


def test_number_status_for_every_setpoint(repo_root, tmp_path):
    """v0.7 : texte d'état facultatif pour chaque consigne (ECS et chauffage)."""

    def status_on(text):
        for address in ("0x2306", "0x2307", "0x3306", "0x3307", "0x6300"):
            old = f"    address: {address}\n"
            assert old in text
            text = text.replace(
                old, old + f'    status:\n      name: "TEST état {address}"\n', 1
            )
        return text

    result = _run_config(repo_root, tmp_path, status_on)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert out.count("name: TEST état 0x") == 5, out


def test_dhw_trial_config_valid_and_without_writes(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path, example=ESSAI_CONSIGNE_ECS)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    # 0x6300 lu une seule fois : le number, bornes 50..60, 60 s, avec son état.
    assert out.count("address: 0x6300\n") == 1, out
    match = re.search(r"name: ECS - Température de consigne\n((?:    [^\n]*\n)*)", out)
    assert match, out
    entry = match.group(1)
    for line in (
        "address: 0x6300",
        "min_value: 50",
        "max_value: 60",
        "update_interval: 60s",
    ):
        assert f"    {line}\n" in entry, line
    assert "name: ECS - État modification consigne" in out, out


def test_dhw_trial_only_changes_the_dhw_number(repo_root):
    example = _active_lines((repo_root / EXAMPLE).read_text(encoding="utf-8"))
    trial = _active_lines((repo_root / ESSAI_CONSIGNE_ECS).read_text(encoding="utf-8"))
    removed = [line for line in example.splitlines() if line not in trial.splitlines()]
    assert removed == ['    name: "ECS - Consigne"', "    min_value: 40"], removed
    added = [line for line in trial.splitlines() if line not in example.splitlines()]
    assert added == [
        '    name: "ECS - Température de consigne"',
        "    min_value: 50",
        # update_interval: 60s existe déjà ailleurs : vérifié par
        # test_dhw_trial_config_valid_and_without_writes.
        "    status:",
        '      name: "ECS - État modification consigne"',
    ], added


@pytest.mark.parametrize(
    "example",
    sorted(
        f"examples/{p.name}"
        for p in (Path(__file__).resolve().parent.parent / "examples").glob("*.yaml")
        if p.name != "secrets.yaml"
    ),
)
def test_examples_declare_no_write_flags(repo_root, example):
    """Configuration minimale : aucun exemple ne déclare les autorisations ni
    les jours du bouclage (défauts : tout autorisé, sept jours)."""
    text = _active_lines((repo_root / example).read_text(encoding="utf-8"))
    for flag in WRITE_FLAGS:
        assert f"{flag}:" not in text, (example, flag)
    assert "validated_days" not in text, example


# --- v0.7 : consignes de chauffage A1/M2 et climate du plancher M2 -----------

ESSAI_CHAUFFAGE = "examples/iot-viessmann-essai-chauffage-v0.7.yaml"
HEATING = {0x2306: (16, 23), 0x2307: (10, 20), 0x3306: (16, 22), 0x3307: (14, 20)}


def test_heating_setpoint_writes_enabled_by_default_and_can_be_disabled(
    repo_root, tmp_path
):
    result = _run_config(repo_root, tmp_path)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert "enable_heating_setpoint_writes: true" in out, out
    assert "HEATING SETPOINT WRITES ENABLED" in out
    off = _run_config(
        repo_root, tmp_path, _with_flags(enable_heating_setpoint_writes="false")
    )
    out = off.stdout + off.stderr
    assert off.returncode == 0, out
    assert "enable_heating_setpoint_writes: false" in out, out
    assert "HEATING SETPOINT WRITES ENABLED" not in out
    assert "DHW SETPOINT WRITE ENABLED" in out  # isolée : l'ECS reste autorisée


def test_heating_setpoint_writes_require_enable_writes(repo_root, tmp_path):
    bad = _run_config(
        repo_root,
        tmp_path,
        _with_flags(enable_writes="false", enable_heating_setpoint_writes="true"),
    )
    assert bad.returncode != 0, bad.stdout
    assert "enable_heating_setpoint_writes requires enable_writes: true" in (
        bad.stdout + bad.stderr
    )


def test_heating_limits_match_cpp(repo_root):
    header = (repo_root / "components/visslink/write_guard.h").read_text(
        encoding="utf-8"
    )
    cpp = {
        int(a, 16): (int(lo), int(hi))
        for a, lo, hi in re.findall(
            r"\{0x([0-9A-Fa-f]{4}), (\d+), (\d+), 0x[0-9A-Fa-f]{4}, (?:true|false)\}",
            header,
        )
    }
    number_py = (repo_root / "components/visslink/number.py").read_text(
        encoding="utf-8"
    )
    block = number_py.partition("HEATING_WRITE_LIMITS = {")[2].partition("}")[0]
    py = {
        int(a, 16): (int(lo), int(hi))
        for a, lo, hi in re.findall(r"0x([0-9A-Fa-f]{4}): \((\d+), (\d+)\)", block)
    }
    assert cpp == py == HEATING
    # Pas les mêmes limites pour les radiateurs et le plancher.
    assert HEATING[0x2306] != HEATING[0x3306]
    assert HEATING[0x2307] != HEATING[0x3307]
    for address, (lo, hi) in HEATING.items():  # dans la plage documentée
        assert WRITABLE[address][0] <= lo < hi <= WRITABLE[address][1]


def test_heating_trial_config_valid_and_without_writes(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path, example=ESSAI_CHAUFFAGE)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    for address in ("0x2306", "0x2307", "0x3306", "0x3307", "0x898", "0x3906"):
        assert out.count(f"address: {address}\n") == 1, address  # une seule lecture
    assert "platform: visslink\n    name: Plancher chauffant\n" in out or (
        "name: Plancher chauffant\n" in out
    ), out
    assert out.count("comfort_setpoint_id: m2_comfort_setpoint") == 1, out
    for name in (
        "Radiateurs - Consigne confort",
        "Radiateurs - Consigne réduite",
        "Plancher - Consigne confort",
        "Plancher - Consigne réduite",
    ):
        assert f"name: {name}\n" in out, name  # noms de production conservés
        assert f"name: {name} - État modification\n" in out, name


def test_heating_trial_only_adds_reads_climate_and_status(repo_root):
    example = _active_lines((repo_root / EXAMPLE).read_text(encoding="utf-8"))
    trial = _active_lines((repo_root / ESSAI_CHAUFFAGE).read_text(encoding="utf-8"))
    removed = [line for line in example.splitlines() if line not in trial.splitlines()]
    # Bornes des number ramenées dans les limites internes ; rien d'autre.
    assert removed == ["    max_value: 24"] * 2, removed  # A1 et M2 confort
    added = [line for line in trial.splitlines() if line not in example.splitlines()]
    allowed = re.compile(
        r"^\s*(- platform: visslink|visslink_id: viessmann|climate:|"
        r"id: (m2_room_temperature|m2_pump|m2_comfort_setpoint|m2_mode|"
        r"bruleur_actif)|"
        r'name: "(Plancher chauffant - Température ambiante \(Vitotrol\)|'
        r"Plancher chauffant - Circulateur|Plancher chauffant|"
        r"Plancher chauffant - État|"
        r'(Radiateurs|Plancher) - Consigne (confort|réduite) - État modification)"|'
        r"address: 0x(0898|3906)|data_type: int16|invalid_values: \[0xFFFF\]|"
        r"min_value: (14|22|23)|max_value: (22|23)|"
        r"status:|comfort_setpoint_id: m2_comfort_setpoint|mode_id: m2_mode|"
        r"current_temperature_id: m2_room_temperature|pump_id: m2_pump|"
        r"burner_id: bruleur_actif)$"
    )
    assert all(allowed.match(line) for line in added), [
        line for line in added if not allowed.match(line)
    ]


def _climate_transform(**changes):
    def transform(text):
        for old, new in changes.items():
            assert old in text, old
            text = text.replace(old, new, 1)
        return text

    return transform


@pytest.mark.parametrize(
    "old, new, message",
    [
        (
            "comfort_setpoint_id: m2_comfort_setpoint",
            "comfort_setpoint_id: a1_comfort",
            "comfort_setpoint_id must be the visslink number of the M2 comfort",
        ),
        (
            "mode_id: m2_mode",
            "mode_id: a1_mode",
            "mode_id must be the visslink select of the M2 operating mode",
        ),
        (
            "current_temperature_id: m2_room_temperature",
            "current_temperature_id: m2_flow",
            "current_temperature_id must be a room temperature, not 0x3900",
        ),
        (
            "burner_id: bruleur_actif",
            "burner_id: m2_pump",
            "burner_id must be the visslink binary_sensor of the burner (0x0842)",
        ),
        (
            "pump_id: m2_pump",
            "pump_id: bruleur_actif",
            "pump_id must be the visslink binary_sensor of the M2 circulation",
        ),
    ],
)
def test_climate_rejects_wrong_sources(repo_root, tmp_path, old, new, message):
    def transform(text):
        # Identifiants supplémentaires sur des entités existantes.
        text = text.replace(
            '    name: "Radiateurs - Consigne confort"\n',
            '    id: a1_comfort\n    name: "Radiateurs - Consigne confort"\n',
            1,
        )
        text = text.replace(
            '    name: "Radiateurs - Mode actuel"\n',
            '    id: a1_mode\n    name: "Radiateurs - Mode actuel"\n',
            1,
        )
        text = text.replace(
            '    name: "Température départ M2"\n',
            '    id: m2_flow\n    name: "Température départ M2"\n',
            1,
        )
        assert old in text
        return text.replace(old, new, 1)

    result = _run_config(repo_root, tmp_path, transform, example=ESSAI_CHAUFFAGE)
    out = result.stdout + result.stderr
    assert result.returncode != 0, out
    assert message in out, out


def test_climate_optional_sources(repo_root, tmp_path):
    def transform(text):
        for line in (
            "    current_temperature_id: m2_room_temperature\n",
            "    pump_id: m2_pump\n",
            "    burner_id: bruleur_actif\n",
            '    status:\n      name: "Plancher chauffant - État"\n',
        ):
            assert line in text
            text = text.replace(line, "", 1)
        return text

    result = _run_config(repo_root, tmp_path, transform, example=ESSAI_CHAUFFAGE)
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.mark.parametrize(
    "example",
    [ESSAI_CHAUFFAGE, "examples/iot-viessmann-ecritures-autorisees.yaml"],
)
def test_climate_m2_state_from_existing_reads(repo_root, tmp_path, example):
    """v0.7.3 : état du plancher à partir des lectures existantes (0x3301,
    0x3906, 0x0842), sans lecture supplémentaire."""
    result = _run_config(repo_root, tmp_path, example=example)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert "burner_id: bruleur_actif" in out, out
    assert "name: Plancher chauffant - État" in out, out
    for address in ("0x3301", "0x3906", "0x842"):
        assert out.count(f"address: {address}\n") == 1, address  # une seule lecture


def test_climate_burner_must_be_a_visslink_binary_sensor(repo_root, tmp_path):
    def transform(text):
        old = "    burner_id: bruleur_actif\n"
        assert old in text
        text = text.replace(old, "    burner_id: other_burner\n", 1)
        return text.replace(
            "binary_sensor:\n",
            "binary_sensor:\n  - platform: template\n    id: other_burner\n"
            '    name: "Autre"\n',
            1,
        )

    result = _run_config(repo_root, tmp_path, transform, example=ESSAI_CHAUFFAGE)
    assert result.returncode != 0, result.stdout + result.stderr


def test_climate_m2_state_documented(repo_root):
    """Les quatre états et la péremption figurent dans le code et la doc."""
    header = (repo_root / "components/visslink/circuit_climate.h").read_text(
        encoding="utf-8"
    )
    for text in ('"Chauffe"', '"Inertie"', '"Arrêt"', '"Inconnu"'):
        assert text in header, text
    assert "STALE_INTERVALS = 3" in header
    assert "STALE_MARGIN_MS = 30000" in header
    doc = (repo_root / "docs/consignes-chauffage-v0.7.md").read_text(encoding="utf-8")
    for text in ("Chauffe", "Inertie", "Arrêt", "Inconnu", "burner_id", "30 s"):
        assert text in doc, text


def test_production_has_no_climate(repo_root):
    """Le climate M2 n'existe que dans la configuration d'essai."""
    text = _active_lines((repo_root / EXAMPLE).read_text(encoding="utf-8"))
    assert "\nclimate:" not in text
    assert "0x0898" not in text


# --- v0.7.1 : configuration explicite « toutes écritures autorisées » --------

ECRITURES = "examples/iot-viessmann-ecritures-autorisees.yaml"


def test_all_writes_example_valid_with_six_flags_and_seven_days(repo_root, tmp_path):
    text = _active_lines((repo_root / ECRITURES).read_text(encoding="utf-8"))
    for flag in WRITE_FLAGS:
        assert f"{flag}:" not in text, flag  # défaut : true
    assert "validated_days" not in text  # défaut : les sept jours
    result = _run_config(repo_root, tmp_path, example=ECRITURES)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    for flag in WRITE_FLAGS:
        assert f"{flag}: true" in out, flag
    assert "SCHEDULE BLOCK WRITES ENABLED" in out, out
    assert "HEATING SETPOINT WRITES ENABLED" in out, out
    for day in SEVEN_DAYS:
        assert f"  - {day}\n" in out, day
    # Trois affichages distincts du bouclage, et la pompe lue toutes les 15 s.
    for name in (
        "Bouclage ECS — Plage inscrite",
        "Bouclage ECS — Temps restant",
        "Bouclage ECS — Restauration en cours",
        "Bouclage ECS — Pompe (état réel, 0x0846)",
    ):
        assert f"name: {name}\n" in out, name
    assert out.count("address: 0x846\n") == 1, out
    assert re.search(
        r"address: 0x846\n(?:    [^\n]*\n)*?    update_interval: 15s\n", out
    ), out


def test_all_writes_example_only_changes_permissions_and_loop_display(repo_root):
    """Mêmes entités, bornes et limites que l'essai v0.7 : seuls les
    affichages du bouclage s'ajoutent."""
    trial = _active_lines((repo_root / ESSAI_CHAUFFAGE).read_text(encoding="utf-8"))
    full = _active_lines((repo_root / ECRITURES).read_text(encoding="utf-8"))
    removed = [line for line in trial.splitlines() if line not in full.splitlines()]
    assert removed == [], removed
    added = [line for line in full.splitlines() if line not in trial.splitlines()]
    allowed = re.compile(
        r"^\s+(- platform: visslink|"
        r"name: \"Bouclage ECS — (Pompe \(état réel, 0x0846\)|Plage inscrite|"
        r"Temps restant|Restauration en cours)\"|"
        r"address: 0x0846|update_interval: 15s|device_class: running|"
        r"range:|remaining:|restoring:)$"
    )
    assert all(allowed.match(line) for line in added), added


def test_boost_range_and_remaining_are_optional_display(repo_root, tmp_path):
    # Production : sans ces entités, valide (test_example_config_is_valid).
    # Le temps restant est en minutes, la plage un texte.
    result = _run_config(repo_root, tmp_path, example=ECRITURES)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert re.search(
        r"remaining:\n(?:    [^\n]*\n)*?    unit_of_measurement: min\n", out
    ), out
    init = (repo_root / "components/visslink_boost/__init__.py").read_text(
        encoding="utf-8"
    )
    assert "cv.Optional(CONF_RANGE)" in init
    assert "cv.Optional(CONF_REMAINING)" in init


# --- v0.8 : éditeur des programmations ------------------------------------------

PROGRAMMATIONS = "examples/iot-viessmann-programmations-v0.8.yaml"
EDITOR_ACTIONS = (
    "visslink_schedule_read",
    "visslink_schedule_set",
    "visslink_schedule_copy",
    "visslink_schedule_clear",
    "visslink_schedule_refresh",
    "visslink_schedule_undo",
)


def test_schedule_editor_example_valid(repo_root, tmp_path):
    text = _active_lines((repo_root / PROGRAMMATIONS).read_text(encoding="utf-8"))
    for flag in WRITE_FLAGS:
        assert f"{flag}:" not in text, flag  # défaut : true, jamais réintroduit
    assert "validated_days" not in text
    result = _run_config(repo_root, tmp_path, example=PROGRAMMATIONS)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    for action in EDITOR_ACTIONS:
        assert f"action: {action}\n" in out, action
    assert "supports_response: optional" in out  # lecture : réponse JSON
    assert out.count("supports_response: status") == 5  # succès / motif du refus
    assert "name: Programmations - État de l'édition" in out
    # Une seule lecture de chaque programmation (aucune lecture en plus).
    for address in ("0x2000", "0x3000", "0x2100", "0x2200"):
        assert out.count(f"address: {address}\n") == 1, address
    assert "SCHEDULE BLOCK WRITES ENABLED" in out


def test_schedule_editor_example_only_adds_the_editor(repo_root):
    """Base : l'exemple « toutes écritures autorisées » ; seuls l'éditeur, ses
    actions et les identifiants des programmations sont ajoutés."""
    base = _active_lines((repo_root / ECRITURES).read_text(encoding="utf-8"))
    new = _active_lines((repo_root / PROGRAMMATIONS).read_text(encoding="utf-8"))
    removed = [line for line in base.splitlines() if line not in new.splitlines()]
    assert removed == ["    components: [visslink, visslink_boost]"], removed
    added = [line for line in new.splitlines() if line not in base.splitlines()]
    allowed = re.compile(
        r"^\s*(components: \[visslink, visslink_boost, visslink_schedule_editor\]|"
        r"actions:|- action: visslink_schedule_[a-z]+|variables:|"
        r"(circuit|days|ranges|expected): string|source: int|then:|- api\.respond:|"
        r"data: !lambda .*|success: !lambda .*|error_message: !lambda .*|"
        r"id: programmation_(a1|m2|ecs|bouclage)|visslink_schedule_editor:|"
        r"id: programmations|visslink_id: viessmann|schedules:|"
        r"- programmation_(a1|m2|ecs|bouclage)|status:|"
        r'name: "Programmations - État de l\'édition")$'
    )
    assert all(allowed.match(line) for line in added), [
        line for line in added if not allowed.match(line)
    ]


@pytest.mark.parametrize(
    "transform, message",
    [
        (
            lambda t: t.replace(
                "    - programmation_bouclage\n", "    - programmation_a1\n", 1
            ),
            "schedules: 0x2000 declared twice",
        ),
        (
            lambda t: t.replace(
                'id: programmation_bouclage\n    name: "Bouclage ECS - Programmation"\n'
                "    address: 0x2200\n",
                'id: programmation_bouclage\n    name: "Bouclage ECS - Programmation"\n'
                "    address: 0x2400\n",
                1,
            ),
            "schedules: must be visslink schedule text_sensors",
        ),
    ],
)
def test_schedule_editor_rejects_wrong_schedules(
    repo_root, tmp_path, transform, message
):
    result = _run_config(repo_root, tmp_path, transform, example=PROGRAMMATIONS)
    out = result.stdout + result.stderr
    assert result.returncode != 0, out
    assert message in out, out


def test_schedule_editor_circuits_match_cpp(repo_root):
    """Mêmes circuits modifiables côté validation (Python) et côté écriture
    (liste blanche C++), et rien d'autre."""
    header = (repo_root / "components/visslink/schedule_editor.h").read_text(
        encoding="utf-8"
    )
    cpp = re.search(
        r"EDITOR_CIRCUIT_ADDRESSES\[EDITOR_CIRCUIT_COUNT\] = \{([^}]*)\}", header
    )
    assert cpp is not None
    addresses = [int(a, 16) for a in re.findall(r"0x([0-9A-Fa-f]{4})", cpp.group(1))]
    init = (repo_root / "components/visslink_schedule_editor/__init__.py").read_text(
        encoding="utf-8"
    )
    py = [
        int(a, 16) for a in re.findall(r"^    0x([0-9A-Fa-f]{4}):", init, re.MULTILINE)
    ]
    assert addresses == py == [0x2000, 0x3000, 0x2100, 0x2200]
    assert "EDITOR_LAST_END_MIN = 23 * 60 + 50" in header


def test_schedule_editor_has_no_permission_options(repo_root):
    """Autorisé par défaut depuis la v0.7.2 : aucune option enable_* ni
    validated_days dans le composant de l'éditeur."""
    init = (repo_root / "components/visslink_schedule_editor/__init__.py").read_text(
        encoding="utf-8"
    )
    assert "cv.Optional(CONF_ENABLE" not in init
    assert "validated_days" not in init.split('"""', 2)[2]


def test_production_example_unchanged_by_the_editor(repo_root):
    text = _active_lines((repo_root / EXAMPLE).read_text(encoding="utf-8"))
    assert "visslink_schedule_editor" not in text
    assert "actions:" not in text


# --- v0.8.1 : diagnostics et cycles du brûleur -----------------------------------

DIAGNOSTICS = "examples/iot-viessmann-diagnostics-v0.8.1.yaml"


def test_diagnostics_example_valid_without_extra_burner_reads(repo_root, tmp_path):
    text = (repo_root / DIAGNOSTICS).read_text(encoding="utf-8")
    assert "A38F" not in text.upper()  # puissance brute : toujours FF FF
    active = _active_lines(text)
    for flag in WRITE_FLAGS:
        assert f"{flag}:" not in active, flag
    assert "validated_days" not in active
    result = _run_config(repo_root, tmp_path, example=DIAGNOSTICS)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    # Registres réutilisés : chacun lu une seule fois, par son entité.
    # (esphome config affiche 0x842 pour 0x0842.)
    for address in ("0x842", "0x88A", "0x8A7", "0x802", "0x5502"):
        assert out.count(f"address: {address}\n") == 1, address
    for name in (
        "Brûleur - Démarrages sur 24 h",
        "Brûleur - Démarrages par heure",
        "Brûleur - Durée moyenne de fonctionnement",
        "Brûleur - Durée du dernier cycle (estimation)",
        "Brûleur - Durée du dernier arrêt (estimation)",
        "Brûleur - Température chaudière au démarrage (estimation)",
        "Brûleur - Température chaudière à l'arrêt (estimation)",
        "Brûleur - Précision des cycles",
        "Consigne chaudière calculée",
        "VissLink - Uptime",
        "VissLink - Signal Wi-Fi",
        "VissLink - Adresse IP",
        "Programmations - État de l'édition",
        "Bouclage ECS — État de l'opération",
    ):
        assert f"name: {name}\n" in out, name


def test_diagnostics_example_only_adds_diagnostics(repo_root):
    """Base : l'exemple v0.8 ; seuls les diagnostics v0.8.1 sont ajoutés."""
    base = _active_lines((repo_root / PROGRAMMATIONS).read_text(encoding="utf-8"))
    new = _active_lines((repo_root / DIAGNOSTICS).read_text(encoding="utf-8"))
    removed = [line for line in base.splitlines() if line not in new.splitlines()]
    assert removed == [
        "    components: [visslink, visslink_boost, visslink_schedule_editor]"
    ], removed
    added = [line for line in new.splitlines() if line not in base.splitlines()]
    allowed = re.compile(
        r"^\s*(components: \[visslink, visslink_boost, visslink_schedule_editor, "
        r"visslink_burner\]|"
        r"id: (temperature_chaudiere|demarrages_bruleur|fonctionnement_bruleur)|"
        r"- platform: (visslink|wifi_signal|uptime|wifi_info)|"
        r'name: "(Consigne chaudière calculée|VissLink - [^"]+|Brûleur - [^"]+)"|'
        r"address: 0x5502|update_interval: (2min|60s)|entity_category: diagnostic|"
        r"(ip_address|ssid):|visslink_burner:|"
        r"(burner|starts|runtime|boiler_temperature)_id: [a-z_]+|"
        r"(starts_24h|starts_per_hour|average_runtime|last_cycle|last_stop|"
        r"start_temperature|stop_temperature|status):)$"
    )
    assert all(allowed.match(line) for line in added), [
        line for line in added if not allowed.match(line)
    ]


@pytest.mark.parametrize(
    "transform, message",
    [
        (
            lambda t: t.replace(
                "  burner_id: bruleur_actif\n", "  burner_id: m2_pump\n"
            ),
            "burner_id must be the visslink binary_sensor of the burner (0x0842)",
        ),
        (
            lambda t: t.replace(
                "  starts_id: demarrages_bruleur\n",
                "  starts_id: temperature_chaudiere\n",
            ),
            "starts_id must be the visslink sensor of the burner starts counter",
        ),
        (
            lambda t: t.replace(
                "    address: 0x08A7\n    data_type: uint32",
                "    address: 0x08A7\n    data_type: uint16",
            ),
            "runtime_id must be the visslink sensor of the burner runtime counter",
        ),
        (
            lambda t: t.replace("  runtime_id: fonctionnement_bruleur\n", ""),
            "'average_runtime' needs 'runtime_id'",
        ),
        (
            lambda t: t.replace("  boiler_temperature_id: temperature_chaudiere\n", ""),
            "'start_temperature' needs 'boiler_temperature_id'",
        ),
    ],
)
def test_burner_cycles_rejects_wrong_sources(repo_root, tmp_path, transform, message):
    result = _run_config(repo_root, tmp_path, transform, example=DIAGNOSTICS)
    out = result.stdout + result.stderr
    assert result.returncode != 0, out
    assert message in out, out


def test_burner_cycles_reads_nothing(repo_root):
    """Le composant n'émet aucune lecture ni écriture Optolink."""
    src = "".join(
        (repo_root / "components/visslink_burner" / name).read_text(encoding="utf-8")
        for name in ("visslink_burner.h", "visslink_burner.cpp")
    )
    src += (repo_root / "components/visslink/burner_cycles.h").read_text(
        encoding="utf-8"
    )
    for forbidden in (
        "request_direct_read",
        "submit_block_write",
        "request_read",
        "request_write",
        "expedite",
        "submit(",
    ):
        assert forbidden not in src, forbidden


def test_setpoint_status_never_starts_unknown(repo_root):
    header = (repo_root / "components/visslink/dhw_setpoint.h").read_text(
        encoding="utf-8"
    )
    assert 'this->status_ = VL_TEXT("Aucune modification effectuée");' in header
    assert "uint32_t version_{1};" in header


def test_boost_duration_select_goes_through_the_component(repo_root):
    """v0.8.1 : le choix de durée passe par le composant (raccourcie, ou
    refusée et ramenée à la durée en cours pendant une opération)."""
    init = (repo_root / "components/visslink_boost/__init__.py").read_text(
        encoding="utf-8"
    )
    assert "cg.add(sel.set_parent(var))" in init
    cpp = (repo_root / "components/visslink_boost/visslink_boost.cpp").read_text(
        encoding="utf-8"
    )
    assert "void BoostDurationSelect::control(size_t index) {" in cpp
    assert "this->parent_->duration_selected(index);" in cpp
    assert "this->sequencer_.change_duration(minutes, now)" in cpp


# --- v0.9 : Éco et Party (lecture et validation, aucune commande) ----------------

ECO_PARTY_V09 = "examples/iot-viessmann-eco-party-v0.9.yaml"
PARTY_SETPOINTS = (0x2308, 0x3308)


def test_eco_party_v09_example_reads_without_commands(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path, example=ECO_PARTY_V09)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    for address in ("0x2302", "0x2303", "0x3302", "0x3303", "0x2308", "0x3308"):
        assert out.count(f"address: {address}\n") == 1, address
    assert "name: Radiateurs - Consigne Party (lecture)" in out
    assert "name: Plancher chauffant - Consigne Party (lecture)" in out
    # Climate M2 inchangé : HEAT/OFF, sans preset Éco ni Party.
    assert "preset" not in out.lower()


def test_eco_party_v09_example_only_adds_party_setpoint_reads(repo_root):
    base = _active_lines((repo_root / DIAGNOSTICS).read_text(encoding="utf-8"))
    new = _active_lines((repo_root / ECO_PARTY_V09).read_text(encoding="utf-8"))
    assert [line for line in base.splitlines() if line not in new.splitlines()] == []
    added = [line for line in new.splitlines() if line not in base.splitlines()]
    allowed = re.compile(
        r"^\s*(- platform: visslink|visslink_id: viessmann|"
        r'name: "(Radiateurs|Plancher chauffant) - Consigne Party \(lecture\)"|'
        r"address: 0x(2308|3308)|data_type: uint8|update_interval: 5min|"
        r'unit_of_measurement: "°C"|device_class: temperature|'
        r"accuracy_decimals: 0)$"
    )
    assert all(allowed.match(line) for line in added), [
        line for line in added if not allowed.match(line)
    ]


@pytest.mark.parametrize("address", PARTY_SETPOINTS)
def test_party_setpoints_never_writable(repo_root, tmp_path, address):
    """Consigne Party : ni number (liste blanche des consignes), ni select."""
    for old, expected in (
        ("address: 0x2306", "not in the VissLink write whitelist"),
        ("address: 0x2301", "is not an operating mode register"),
    ):
        result = _run_config(
            repo_root,
            tmp_path,
            lambda s, old=old: s.replace(old, f"address: 0x{address:04X}"),
        )
        out = result.stdout + result.stderr
        assert result.returncode != 0, hex(address)
        assert expected in out, out


def test_climate_has_no_eco_party_preset(repo_root):
    """Presets Éco/Party seulement après confirmation des commandes Optolink
    sur la KW3 (docs/eco-party-v0.9.md) : aucun aujourd'hui."""
    for path in (
        "components/visslink/climate.py",
        "components/visslink/visslink.cpp",
        "components/visslink/visslink.h",
        "components/visslink/circuit_climate.h",
    ):
        text = (repo_root / path).read_text(encoding="utf-8")
        assert "preset" not in text.lower(), path


def test_eco_party_registers_absent_from_cpp_whitelists(repo_root):
    """Aucune liste blanche C++ (consignes, programmes, blocs) ne contient
    Éco, Party ou les consignes Party."""
    code = ""
    for path in (
        "components/visslink/write_guard.h",
        "components/visslink/circulation_boost.h",
        "components/visslink/schedule_editor.h",
    ):
        text = (repo_root / path).read_text(encoding="utf-8")
        code += re.sub(r"//[^\n]*", "", text).upper()
    for address in ("0x2302", "0x2303", "0x3302", "0x3303", "0x2308", "0x3308"):
        assert address.upper() not in code, address
