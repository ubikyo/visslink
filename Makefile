# Raccourcis de développement VissLink.
EXAMPLE := examples/iot-viessmann.yaml

.PHONY: test cpp-test lint config compile secrets

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
