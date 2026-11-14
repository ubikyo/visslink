# VissLink

Composant externe [ESPHome](https://esphome.io) pour lire une régulation
**Viessmann Vitotronic** via son interface optique **Optolink**, en protocole
**KW**, et publier les mesures dans Home Assistant.
## Configuration

Voir l'exemple complet [examples/iot-viessmann.yaml](examples/iot-viessmann.yaml).
Les identifiants restent dans `secrets.yaml` (non versionné).

Chaque registre est une entité `sensor` ou `binary_sensor` avec son propre
`update_interval` (1 s à 7 jours, 60 s par défaut). Toutes les entités partagent
un seul ordonnanceur : une requête à la fois, aucune en double (une adresse ne
peut être déclarée qu'une fois, toutes plateformes confondues).

### `sensor` : valeurs numériques

```yaml
sensor:
  - platform: visslink
    name: "Température extérieure"
    address: 0x0800
    data_type: int16        # défaut : int16 (compatible v0.1)
    update_interval: 60s
    filters:
      - multiply: 0.1
```

| `data_type` | Octets lus | Plage |
|---|---|---|
| `uint8` | 1 | 0 … 255 |
| `int16` | 2 | −32 768 … 32 767 |
| `uint16` | 2 | 0 … 65 535 |
| `uint32` | 4 | 0 … 4 294 967 295 |

Les valeurs sont transmises octet de poids faible en premier (little-endian) :
`CC FF` vaut −52 en `int16` et 65 484 en `uint16`. Le composant publie la valeur
brute ; l'échelle se règle par les filtres ESPHome (`multiply: 0.1` pour les
températures, `multiply: 0.00027777778` pour convertir des secondes en heures).

Les capteurs ESPHome sont des `float` : un `uint32` reste exact jusqu'à
16 777 216 et perd ensuite au plus quelques unités (sans effet une fois
converti en heures).

### `binary_sensor` : états

```yaml
binary_sensor:
  - platform: visslink
    name: "Brûleur actif"
    address: 0x0842
    update_interval: 30s
```

Un octet est lu et décodé strictement :

| Octet reçu | État publié |
|---|---|
| `00` | off |
| `01` | on |
| autre | inconnu, avec un avertissement donnant l'octet reçu |

Aucune autre valeur n'est interprétée (pas de « non nul = on ») : un octet
inattendu signale un registre mal identifié ou un octet d'état multi-bits.

### Vérifier un registre expérimental

1. Le déclarer en `sensor` `data_type: uint8` et observer ses valeurs pendant
   quelques jours, ainsi que les journaux DEBUG (`0x0846 <- 01`), qui montrent
   toujours les octets bruts reçus.
2. Ne le passer en `binary_sensor` (en retirant l'entrée `uint8`) que si seules
   les valeurs `00` et `01` apparaissent et qu'elles suivent le fonctionnement
   réel de l'installation.

Les registres expérimentaux de l'exemple sont en commentaire.

### Charge de la liaison

Chaque lecture attend le caractère de synchro `0x05` de la régulation, soit
environ 2 à 3 s : la liaison supporte 20 à 30 lectures par minute. Les premières
échéances sont décalées de 2 s par registre au démarrage, pour éviter que tous
les registres reviennent en rafale au même instant. Au-delà de la capacité, les
intervalles s'allongent sans accumulation de requêtes ni registre oublié.

## Feuille de route

- **v0.2.1** : programmations horaires (blocs de 8 octets, 4 plages par jour,
  7 jours, publication en `text_sensor`, intervalle 6 h) pour `0x2000` (A1),
  `0x3000` (M2), `0x2100` (production ECS), `0x2200` (bouclage ECS). Pas avant
  la validation sur la chaudière des capteurs numériques et binaires v0.2.
