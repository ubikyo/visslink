# Raccourcis de développement VissLink.
EXAMPLE := examples/iot-viessmann.yaml

.PHONY: test cpp-test lint config compile secrets compat-v01

# Tests C++ + validation de configuration ESPHome
test:
	pytest -v

# Tests C++ seuls (avec AddressSanitizer / UBSan)
cpp-test:
	cmake -S tests/cpp -B tests/cpp/build -G Ninja
	cmake --build tests/cpp/build
	./tests/cpp/build/visslink_tests

lint:
	ruff check components tests
	ruff format --check components tests

# Crée un examples/secrets.yaml FICTIF (ignoré par git) s'il n'existe pas.
secrets:
	@test -f examples/secrets.yaml || { \
	  printf 'wifi_ssid: "dummy"\nwifi_password: "dummy-password"\nfallback_ap_password: "dummy-fallback"\napi_encryption_key: "%s"\n' \
	    "$$(openssl rand -base64 32)" > examples/secrets.yaml; \
	  echo "examples/secrets.yaml fictif créé"; }

config: secrets
	esphome config $(EXAMPLE)

compile: secrets
	esphome compile $(EXAMPLE)

# Rétrocompatibilité : le YAML de la v0.1.0, inchangé, doit rester valide et
# compiler (sensor seul, sans binary_sensor). Construit dans build/compat-v01
# avec des secrets fictifs ; examples/secrets.yaml n'est pas utilisé.
COMPAT_DIR := build/compat-v01
compat-v01:
	mkdir -p $(COMPAT_DIR)
	sed 's|path: ../components|path: $(CURDIR)/components|' \
	  tests/fixtures/iot-viessmann-v0.1.yaml > $(COMPAT_DIR)/iot-viessmann.yaml
	printf 'wifi_ssid: "dummy"\nwifi_password: "dummy-password"\nfallback_ap_password: "dummy-fallback"\napi_encryption_key: "%s"\n' \
	  "$$(openssl rand -base64 32)" > $(COMPAT_DIR)/secrets.yaml
	esphome config $(COMPAT_DIR)/iot-viessmann.yaml
	esphome compile $(COMPAT_DIR)/iot-viessmann.yaml
