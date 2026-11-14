# VissLink

Composant externe [ESPHome](https://esphome.io) pour lire une régulation
**Viessmann Vitotronic** via son interface optique **Optolink**, en protocole
**KW**, et publier les mesures dans Home Assistant.
## Configuration

Voir l'exemple complet [examples/iot-viessmann.yaml](examples/iot-viessmann.yaml).
Les identifiants restent dans `secrets.yaml` (non versionné).

Chaque registre est une entité `sensor`, `binary_sensor` ou `text_sensor`
(programmation horaire) avec son propre `update_interval` (1 s à 7 jours ; 60 s
par défaut, 6 h pour les programmations). Toutes les entités partagent un seul
ordonnanceur : une requête à la fois, aucune en double (une adresse ne peut être
déclarée qu'une fois, toutes plateformes confondues ; les 7 blocs d'une
programmation comptent chacun).

VissLink est strictement en **lecture seule** : aucune commande d'écriture
Optolink n'existe dans le code.

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

Les registres expérimentaux de l'exemple sont en commentaire. Pour les six
registres encore à identifier (`0x0846`, `0x0845`, `0x2906`, `0x3906`,
`0x0883`, `0x5502`), la configuration
[examples/iot-viessmann-diagnostic.yaml](examples/iot-viessmann-diagnostic.yaml)
les lit en valeurs brutes. Le statut de chaque registre, les procédures de
validation et la charge de la liaison sont décrits dans
[docs/registres.md](docs/registres.md). Validation recommandée : un groupe de
deux registres à la fois (G1 ECS `0x0846`/`0x0845`, G2 circulateurs
`0x2906`/`0x3906`, G3 défaut et consigne `0x0883`/`0x5502`).

### `text_sensor` : programmations horaires

Une entité par circuit. `address` est le bloc du **lundi** ; les 7 jours sont
lus à la suite.

```yaml
time:
  - platform: homeassistant
    id: ha_time

text_sensor:
  - platform: visslink
    name: "Bouclage ECS - Programmation"
    address: 0x2200
    update_interval: 6h          # défaut
    time_id: ha_time             # requis seulement pour today / active
    today:                       # facultatif
      name: "Bouclage ECS - Programmation du jour"
    active:                      # facultatif (binary_sensor)
      name: "Bouclage ECS - Plage active"
    # monday: ... sunday:        # facultatif : un capteur texte par jour
    #   name: "Bouclage ECS - Lundi"
```

| Entité | Contenu | Exemple |
|---|---|---|
| principale | semaine, jours consécutifs identiques regroupés | `Lu-Ve 06:00-22:00 \| Sa-Di 07:00-23:00` |
| `today` | jour courant (heure locale) | `06:10-08:30, 21:40-21:50` |
| `active` | une plage programmée est en cours | on / off |
| `monday` … `sunday` | un jour | `aucune plage` |

Textes particuliers : `aucune plage` (jour lu, sans plage programmée), `non lu`
(jour jamais lu avec succès). Tant que la première lecture des 7 jours n'est pas
terminée, l'entité principale reste « inconnu » plutôt que d'afficher une
semaine partielle.

#### Registres

| Circuit | Lundi | Dimanche | Statut |
|---|---|---|---|
| Radiateurs A1 | `0x2000` | `0x2030` | 7 blocs lus et décodés |
| Plancher chauffant M2 | `0x3000` | `0x3030` | 7 blocs lus et décodés |
| Production ECS | `0x2100` | `0x2130` | 7 blocs lus et décodés |
| Bouclage ECS | `0x2200` | `0x2230` | 7 blocs lus et décodés |

Adresse d'un jour : `adresse_lundi + 8 × jour` (lundi = 0 … dimanche = 6).
Sur la chaudière réelle, les **sept blocs** des quatre circuits sont lus et
décodés correctement (trames bien cadrées, plages valides). La correspondance
de chaque bloc avec son jour (`+ 8 × jour`) n'a en revanche **pas été vérifiée
jour par jour** : seul le bloc du lundi a été comparé individuellement à la
régulation (voir ci-dessous).

#### Format d'un bloc (8 octets)

```text
DEBUT1 FIN1 DEBUT2 FIN2 DEBUT3 FIN3 DEBUT4 FIN4
heures = octet / 8      minutes = (octet % 8) × 10      FF FF = plage libre
```

| Trame relevée | Programmation |
|---|---|
| `30 B0 FF FF FF FF FF FF` | 06:00-22:00 |
| `20 B8 FF FF FF FF FF FF` | 04:00-23:00 |
| `31 43 AC AD FF FF FF FF` | 06:10-08:30, 21:40-21:50 |

Un bloc est **rejeté en entier** (aucune valeur publiée, la dernière valeur
valide est conservée, avertissement dans les journaux) si :

- la réponse ne fait pas exactement 8 octets (contrôlé par le moteur KW) ;
- une paire est incomplète (`FF` d'un seul côté) ;
- un octet n'est pas une heure : dizaine de minutes 6 ou 7 (ex. `0x36`), ou
  au-delà de 24:00 (`0xC0`) ;
- une plage commence à 24:00 ou finit avant (ou à) son début. Aucune convention
  de plage traversant minuit n'est supposée.

Un bloc rejeté est retenté comme un échec de communication (1 s plus tard,
`max_retries` fois), puis le circuit passe au jour suivant.

#### Plage active

`active` est calculé sur l'ESP à partir du jour et de l'heure locale du
composant `time` (réévalué chaque seconde) et des plages lues : début inclus,
fin exclue. Il vaut **inconnu** tant que l'heure n'est pas synchronisée ou que
le jour courant n'a pas été lu ; il n'est jamais déduit d'un autre jour.

Une plage active signifie seulement que la régulation **autorise** le
fonctionnement du circuit à cet instant. Elle ne dit pas que le brûleur
(`binary_sensor` `0x0842`) ou une pompe tourne réellement : la régulation peut
rester à l'arrêt en plage (consigne atteinte, été…) ou fonctionner hors plage
(hors-gel, mode réduit, charge ECS prioritaire). Pour cette raison l'entité n'a
pas de `device_class: running` par défaut.

#### Choix de présentation et limites

- ESPHome ne permet pas d'attacher des **attributs** à une entité. Plutôt
  qu'un contournement (JSON dans l'état, appels de service), l'état principal
  est la semaine résumée : dans le cas courant (un horaire identique en semaine
  et un autre le week-end) elle tient en quelques dizaines de caractères.
- L'état d'une entité Home Assistant est limité à **255 caractères**. Le
  résumé le respecte toujours ; au-delà (jours tous différents avec plusieurs
  plages, jusqu'à 368 caractères), il est tronqué et se termine par `...`. Les
  capteurs `monday` … `sunday` (50 caractères au plus) donnent alors le détail
  exact.
- Pour une présentation sur plusieurs lignes, une carte Markdown Home
  Assistant peut afficher `today` ou les capteurs par jour.
- Après un échec de lecture, la programmation **conserve** sa dernière valeur
  valide (elle ne périme pas comme une mesure). Une modification faite sur la
  régulation n'apparaît qu'à la lecture suivante (6 h par défaut).
- La valeur `0xC0` (24:00) est acceptée en fin de plage d'après la règle de
  codage ; elle n'a pas été observée sur cette chaudière.

#### Vérifier la correspondance des jours

La lecture et le décodage des sept blocs sont acquis. Reste à vérifier, si
besoin, que chaque bloc correspond bien au jour attendu :

1. Lire dans les journaux DEBUG les trames brutes, par exemple
   `0x2008 <- 30 B0 FF FF FF FF FF FF` pour le mardi d'A1.
2. Modifier temporairement une plage d'un seul jour sur la régulation (par
   exemple le jeudi d'A1) et vérifier que seul le bloc attendu (`0x2018`)
   change à la lecture suivante.
3. Comparer le résumé publié avec l'affichage de la régulation pour les 4
   circuits.

### Charge de la liaison

Chaque lecture attend le caractère de synchro `0x05` de la régulation, soit
environ 2 à 3 s : la liaison supporte 20 à 30 lectures par minute. Les premières
échéances sont décalées de 2 s par registre au démarrage, pour éviter que tous
les registres reviennent en rafale au même instant. Au-delà de la capacité, les
intervalles s'allongent sans accumulation de requêtes ni registre oublié.

Une programmation occupe **une seule entrée** de l'ordonnanceur pour ses 7
blocs : après chaque bloc, elle revient 30 s plus tard (en concurrence normale
avec les autres registres) et ne reprend son intervalle de 6 h qu'après le
dimanche. Les 4 circuits ajoutent donc 4 entrées (et non 28), soit 28 lectures
étalées sur environ 4 minutes toutes les 6 h, ~0,08 lecture/min en moyenne.
Le registre le plus en retard reste servi en premier : un registre échu attend
au plus N−1 lectures, N étant le nombre d'entrées (16 dans l'exemple).

Simulations (chaudière simulée, 12 registres actifs de l'exemple + 4
programmations, délai de resynchro 2 à 4 s, sur 6 h 30) : le brûleur à 30 s ne
perd aucun cycle et son écart maximal entre deux lectures est identique à celui
de la même configuration sans programmation. À titre de comparaison, les 28
blocs déclarés comme registres indépendants reviendraient en rafale et
priveraient le brûleur de lecture pendant ~90 s toutes les 6 h (test
`sim_v021_28_independent_registers_would_starve_burner`). Avec les 18 registres
de l'exemple, expérimentaux compris, et une chaudière lente (resynchro ≥ 3,5 s),
la liaison est déjà saturée sans programmation : toute charge supplémentaire y
allonge les intervalles.

### Empreinte mémoire (Wemos D1 mini, ESPHome 2026.10.2)

| Firmware | RAM statique | Flash |
|---|---|---|
| v0.2, exemple v0.2 | 31 960 o (39,0 %) | 423 981 o (40,6 %) |
| v0.2.1, exemple v0.2 inchangé | 31 980 o (+20 o) | 424 353 o (+372 o) |
| v0.2.1, exemple complet | 33 796 o (+1 836 o, 41,3 %) | 434 713 o (+10,7 Ko, 41,6 %) |

Sans programmation, le surcoût de la v0.2.1 est négligeable. L'essentiel des
+1,8 Ko de l'exemple complet vient des 12 nouvelles entités (4 programmations,
4 « jour », 4 « plage active ») et des composants ESPHome `text_sensor` et
`time`. Côté VissLink, chaque circuit conserve sa semaine décodée (~130 o) et
chaque entrée de l'ordonnanceur passe de 12 à 16 o. Les textes publiés (au plus
255 o, ~20 à 60 o en pratique) ne sont reconstruits qu'après une lecture, ou
chaque seconde pour `today` (publié seulement s'il change).

## Développement

```sh
make test         # tests C++ (ASan/UBSan) + validation de configuration ESPHome
make lint
make compile      # firmware d1_mini de l'exemple (secrets fictifs)
make compat       # YAML v0.1.0 et v0.2 inchangés : validation + compilation
```

## Historique

- **v0.2.2** : configuration de diagnostic des registres expérimentaux
  (valeurs brutes, lecture seule), procédures de validation et simulation de
  charge ([docs/registres.md](docs/registres.md)). Composant inchangé.
- **v0.2.1** : programmations horaires en lecture seule (`text_sensor`), jour
  courant et plage active calculés sur l'ESP, ordonnanceur multi-blocs.
- **v0.2** : types numériques (`uint8`, `int16`, `uint16`, `uint32`) et
  capteurs binaires.
- **v0.1** : températures `int16`.
