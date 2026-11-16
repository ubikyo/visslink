"""Validation de la configuration ESPHome (`esphome config`).

L'exemple est copié dans un dossier temporaire avec des secrets fictifs générés
à la volée : aucun identifiant n'est écrit dans le dépôt.
"""

import ast
import base64
import os
import re
import subprocess

import pytest

EXAMPLE = "examples/iot-viessmann.yaml"
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
    "enable_writes_not_boolean": lambda s: s.replace(
        "enable_writes: false", "enable_writes: maybe"
    ),
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
    "mode_writes_without_writes": lambda s: s.replace(
        "enable_mode_writes: false", "enable_mode_writes: true"
    ),
    "mode_writes_not_boolean": lambda s: s.replace(
        "enable_mode_writes: false", "enable_mode_writes: perhaps"
    ),
    # v0.4 : M2 exige enable_writes ET enable_mode_writes.
    "m2_write_alone": lambda s: s.replace(
        "enable_m2_mode_write: false", "enable_m2_mode_write: true"
    ),
    "m2_write_with_writes_only": lambda s: s.replace(
        "enable_m2_mode_write: false", "enable_m2_mode_write: true"
    ).replace("enable_writes: false", "enable_writes: true"),
    "m2_write_with_mode_writes_only": lambda s: s.replace(
        "enable_m2_mode_write: false", "enable_m2_mode_write: true"
    ).replace("enable_mode_writes: false", "enable_mode_writes: true"),
    "m2_write_not_boolean": lambda s: s.replace(
        "enable_m2_mode_write: false", "enable_m2_mode_write: maybe"
    ),
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


def test_example_writes_disabled_by_default(repo_root, tmp_path):
    text = (repo_root / EXAMPLE).read_text(encoding="utf-8")
    assert "  enable_writes: false\n" in text
    assert "enable_writes: true" not in text
    # Sans l'option : désactivées aussi (défaut).
    result = _run_config(
        repo_root, tmp_path, lambda s: _drop_first(s, "  enable_writes: false\n")
    )
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert "enable_writes: false" in out, out
    assert "WRITES ENABLED" not in out


def test_enable_writes_is_explicit_and_warned(repo_root, tmp_path):
    result = _run_config(
        repo_root,
        tmp_path,
        lambda s: s.replace("enable_writes: false", "enable_writes: true"),
    )
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert "WRITES ENABLED" in out, out


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
    py_table = {
        int(a, 16): (int(lo), int(hi))
        for a, lo, hi in re.findall(r"0x([0-9A-Fa-f]{4}): \((\d+), (\d+)\)", py)
    }
    assert cpp_table == WRITABLE
    assert py_table == WRITABLE


# --- Compatibilité v0.2.1 / v0.2.2 ---------------------------------------------


@pytest.mark.parametrize("fixture", [EXAMPLE_V022, DIAGNOSTIC_V022])
def test_v022_configs_still_valid(repo_root, tmp_path, fixture):
    result = _run_config(repo_root, tmp_path, example=fixture)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    # Sans `enable_writes`, la v0.2.2 reste en lecture seule.
    assert "enable_writes: false" in out, out
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


def test_example_mode_writes_disabled_by_default(repo_root, tmp_path):
    text = (repo_root / EXAMPLE).read_text(encoding="utf-8")
    assert "  enable_mode_writes: false\n" in text
    result = _run_config(
        repo_root,
        tmp_path,
        lambda s: _drop_first(s, "  enable_mode_writes: false\n"),
    )
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert "enable_mode_writes: false" in out, out
    assert "OPERATING MODE WRITES ENABLED" not in out


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


def test_example_three_write_flags_false_by_default(repo_root, tmp_path):
    text = (repo_root / EXAMPLE).read_text(encoding="utf-8")
    for flag in ("enable_writes", "enable_mode_writes", "enable_m2_mode_write"):
        assert f"  {flag}: false\n" in text, flag
        assert f"{flag}: true" not in text, flag

    def transform(s):
        for flag in ("enable_writes", "enable_mode_writes", "enable_m2_mode_write"):
            s = _drop_first(s, f"  {flag}: false\n")
        return s

    result = _run_config(repo_root, tmp_path, transform)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    for flag in ("enable_writes", "enable_mode_writes", "enable_m2_mode_write"):
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


def test_loop_diagnostic_is_read_only_and_valid(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path, example=DIAGNOSTIC_LOOP)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    for flag in ("enable_writes", "enable_mode_writes", "enable_m2_mode_write"):
        assert f"{flag}: false" in out, flag
    assert "ENABLED" not in out
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
    assert removed == [], removed
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


def test_example_boost_present_but_inactive(repo_root, tmp_path):
    text = (repo_root / EXAMPLE).read_text(encoding="utf-8")
    assert "  enable_schedule_writes: false\n" in text
    assert "enable_schedule_writes: true" not in text
    result = _run_config(repo_root, tmp_path)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert "visslink_boost:" in out
    assert "validated_days: []" in out, out
    assert "SCHEDULE BLOCK WRITES ENABLED" not in out
    # Horloge lue en lecture seule, à 0x088E.
    assert re.search(
        r"name: Vitotronic - Horloge\n(?:    [^\n]*\n)*?    address: 0x88E\n", out
    ), out


def test_schedule_writes_need_enable_writes(repo_root, tmp_path):
    bad = _run_config(
        repo_root,
        tmp_path,
        lambda s: s.replace(
            "enable_schedule_writes: false", "enable_schedule_writes: true"
        ),
    )
    assert bad.returncode != 0, bad.stdout
    good = _run_config(
        repo_root,
        tmp_path,
        lambda s: s.replace(
            "enable_schedule_writes: false", "enable_schedule_writes: true"
        ).replace("enable_writes: false", "enable_writes: true"),
    )
    out = good.stdout + good.stderr
    assert good.returncode == 0, out
    assert "SCHEDULE BLOCK WRITES ENABLED" in out, out


@pytest.mark.parametrize(
    "case",
    [
        lambda s: s.replace("validated_days: []", "validated_days: [dimanche]"),
        lambda s: s.replace("  time_id: ha_time\n  validated_days", "  validated_days"),
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
        lambda s: s.replace("validated_days: []", "validated_days: [sunday, Monday]"),
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


def test_read_only_trial_config_valid_and_without_writes(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path, example=ESSAI_V05)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    for flag in (
        "enable_writes",
        "enable_mode_writes",
        "enable_m2_mode_write",
        "enable_schedule_writes",
    ):
        assert f"{flag}: false" in out, flag
    assert "validated_days: []" in out
    assert "ENABLED" not in out
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
    for flag in (
        "enable_writes",
        "enable_mode_writes",
        "enable_m2_mode_write",
        "enable_schedule_writes",
    ):
        assert f"{flag}: false" in out, flag
    assert "validated_days: []" in out
    assert "ENABLED" not in out
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
WRITE_FLAGS = (
    "enable_writes",
    "enable_mode_writes",
    "enable_m2_mode_write",
    "enable_schedule_writes",
    "enable_dhw_setpoint_write",
)


def test_dhw_setpoint_write_disabled_by_default(repo_root, tmp_path):
    text = (repo_root / EXAMPLE).read_text(encoding="utf-8")
    assert "  enable_dhw_setpoint_write: false\n" in text
    assert "enable_dhw_setpoint_write: true" not in text
    result = _run_config(
        repo_root,
        tmp_path,
        lambda s: _drop_first(s, "  enable_dhw_setpoint_write: false\n"),
    )
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert "enable_dhw_setpoint_write: false" in out, out
    assert "DHW SETPOINT WRITE ENABLED" not in out


def test_dhw_setpoint_write_requires_enable_writes(repo_root, tmp_path):
    bad = _run_config(
        repo_root,
        tmp_path,
        lambda s: s.replace(
            "enable_dhw_setpoint_write: false", "enable_dhw_setpoint_write: true"
        ),
    )
    assert bad.returncode != 0, bad.stdout
    assert "enable_dhw_setpoint_write requires enable_writes: true" in (
        bad.stdout + bad.stderr
    )
    good = _run_config(
        repo_root,
        tmp_path,
        lambda s: s.replace(
            "enable_dhw_setpoint_write: false", "enable_dhw_setpoint_write: true"
        ).replace("enable_writes: false", "enable_writes: true"),
    )
    out = good.stdout + good.stderr
    assert good.returncode == 0, out
    assert "DHW SETPOINT WRITE ENABLED" in out, out
    # Aucune autre autorisation n'est activée par la sienne.
    for flag in WRITE_FLAGS[1:4]:
        assert f"{flag}: false" in out, flag


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


def test_number_status_only_for_dhw_setpoint(repo_root, tmp_path):
    def status_on(address):
        def transform(text):
            old = f"    address: {address}\n"
            assert old in text
            return text.replace(old, old + '    status:\n      name: "TEST état"\n', 1)

        return transform

    bad = _run_config(repo_root, tmp_path, status_on("0x2306"))
    assert bad.returncode != 0, bad.stdout
    assert "status is only available for the DHW setpoint (0x6300)" in (
        bad.stdout + bad.stderr
    )
    good = _run_config(repo_root, tmp_path, status_on("0x6300"))
    assert good.returncode == 0, good.stdout + good.stderr


def test_dhw_trial_config_valid_and_without_writes(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path, example=ESSAI_CONSIGNE_ECS)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    for flag in WRITE_FLAGS:
        assert f"{flag}: false" in out, flag
    assert "validated_days: []" in out
    assert "ENABLED" not in out
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
    [EXAMPLE, ESSAI_V05, ESSAI_BOUCLAGE, ESSAI_CONSIGNE_ECS],
)
def test_examples_keep_all_write_flags_false(repo_root, example):
    text = (repo_root / example).read_text(encoding="utf-8")
    active = _active_lines(text)
    for flag in WRITE_FLAGS:
        assert f"  {flag}: false" in active.splitlines(), (example, flag)
        assert f"{flag}: true" not in active, (example, flag)
    assert "  validated_days: []" in text
