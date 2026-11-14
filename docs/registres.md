# Registres KW : statut et validation (v0.2.2)

Installation de référence : Vitocrossal 300 CU03, Vitotronic 300 KW3,
Vitocell 300-V, circuits A1 (radiateurs) et M2 (plancher chauffant).

Un registre n'est **confirmé** que lorsque sa valeur a été comparée au
fonctionnement réel de cette installation. Les adresses issues de
documentations communautaires restent **expérimentales** tant que cette
comparaison n'est pas faite.

## 1. Statut des registres

### Confirmés

| Adresse | Fonction | Type | Échelle | Observation |
|---|---|---|---|---|
| `0x0800` | Température extérieure | int16 | × 0,1 °C | lecture cohérente |
| `0x0802` | Température chaudière | int16 | × 0,1 °C | lecture cohérente |
| `0x0804` | Température ECS | int16 | × 0,1 °C | lecture cohérente |
| `0x3900` | Température départ M2 | int16 | × 0,1 °C | lecture cohérente |
| `0x2306` / `0x2307` | Consignes A1 confort / réduit | uint8 | °C | lecture cohérente |
| `0x3306` / `0x3307` | Consignes M2 confort / réduit | uint8 | °C | lecture cohérente |
| `0x6300` | Consigne ECS | uint8 | °C | lecture cohérente |
| `0x088A` | Démarrages brûleur | uint32 | — | lecture cohérente |
| `0x08A7` | Heures brûleur | uint32 | s | lecture cohérente |
| `0x0842` | Brûleur | 1 octet | `00` arrêt, `01` marche | **les deux états observés** |
| `0x2000`, `0x3000`, `0x2100`, `0x2200` | Programmations A1, M2, ECS, bouclage | 7 × 8 octets | voir README | 7 blocs lus et décodés ; correspondance des jours non vérifiée un par un |

### Expérimentaux (v0.2.2)

| Adresse | Fonction supposée | Lecture de diagnostic | Ce qui reste à établir |
|---|---|---|---|
| `0x0846` | Pompe de bouclage ECS | uint8 brut, 60 s | valeurs possibles ; correspondance avec la pompe |
| `0x0845` | Pompe de charge ECS | uint8 brut, 60 s | idem, pendant et hors d'un réchauffage |
| `0x2906` | Circulateur radiateurs A1 | uint8 brut, 2 min | état ou vitesse ? correspondance |
| `0x3906` | Circulateur plancher M2 | uint8 brut, 2 min | idem |
| `0x0883` | Défaut chaudière | uint8 brut, 5 min | adresse **et** codage (indicateur, code, bits ?) |
| `0x5502` | Consigne chaudière calculée | int16 brut, 2 min | adresse, longueur et facteur d'échelle |

Règles appliquées tant qu'un registre est expérimental :

- la valeur est publiée **brute** (`sensor`), sans filtre, unité ni état binaire ;
- une valeur non nulle ne signifie pas qu'une pompe tourne ;
- une plage horaire active ne signifie pas qu'une pompe tourne ;
- un registre de défaut n'utilise pas forcément 0 et 1.

## 2. Mise en œuvre

### Configuration

[`examples/iot-viessmann-diagnostic.yaml`](../examples/iot-viessmann-diagnostic.yaml)
reprend la configuration de production v0.2.1 **sans aucune modification** :
même nœud, mêmes Wi-Fi, API, OTA, UART, registres et programmations. Elle y
ajoute six capteurs `TEST — …` en `entity_category: diagnostic`. Un test
(`test_diagnostic_config_is_the_example_plus_test_sensors`) garantit que seules
ces six entrées diffèrent.

Le composant n'a pas été modifié : `sensor` avec `data_type: uint8` ou `int16`
publie déjà la valeur brute, et chaque lecture est journalisée.

Flasher ce fichier **remplace** le firmware de production (même nom de nœud).
Au retour à `examples/iot-viessmann.yaml`, supprimer dans Home Assistant les
six entités TEST devenues orphelines.

### Ce que montrent les journaux

Niveau `DEBUG`, déjà actif dans l'exemple :

```text
[D][visslink]: 0x0846 <- 01                    lecture réussie : octets bruts reçus
[D][visslink]: 0x5502 <- 4A 02                 int16 little-endian : 0x024A = 586
[W][visslink]: 0x0883: response timeout (attempt 1/3)
[W][visslink]: 0x0883: giving up for this cycle  -> entité « inconnu » dans HA
```

Causes possibles d'échec : `sync timeout` (aucun 0x05 : liaison optique),
`response timeout`, `response too short`, `response too long`.

Les journaux ne sont **pas conservés** par l'ESP ni par Home Assistant. Pour
une observation de plusieurs jours, les enregistrer depuis un PC :

```sh
esphome logs examples/iot-viessmann-diagnostic.yaml | tee visslink-diag.log
```

Les valeurs des entités TEST, elles, sont historisées par Home Assistant (graphe
de l'entité, ou export de l'historique) : c'est la source principale de
l'analyse.

Comptages utiles sur un fichier de journal (pas besoin de compteurs dans le
firmware) :

```sh
grep -c ' <- ' visslink-diag.log                               # lectures réussies
grep -oE '0x[0-9A-F]{4}: [a-z ]+ \(attempt' visslink-diag.log \
  | sort | uniq -c                                             # échecs par registre et cause
grep -c 'attempt [2-9]/' visslink-diag.log                     # nouvelles tentatives
grep -c 'giving up' visslink-diag.log                          # cycles abandonnés
grep ' 0x0846 <- ' visslink-diag.log | awk '{print $NF}' | sort | uniq -c   # valeurs vues
```

### Mesurer le délai de resynchronisation

Au démarrage, les premières échéances des registres sont décalées de 2 s : ils
sont lus à la suite pendant environ une minute. Si l'écart entre deux lignes
` <- ` consécutives de cette première minute dépasse 2 s, il donne le délai
réel entre une réponse et le 0x05 suivant (simulé de 2 à 4 s). Ce délai décide
de la stratégie :

La stratégie recommandée reste la validation **par groupes de deux** (§ 4,
« Diagnostic par groupes ») ; le délai indique seulement si les six registres
peuvent, à titre exceptionnel, être lus ensemble :

| Délai observé | Six registres ensemble |
|---|---|
| ≤ 3 s | possible, sans effet mesurable sur le brûleur |
| 3,5 à 4 s | à éviter : quelques lectures du brûleur retardées |

## 3. Procédures de validation

Pour chaque registre : noter la date, l'heure, la valeur brute (HA ou journal)
et l'état réel constaté. Une validation repose sur des **observations
réelles**, jamais sur la seule programmation horaire.

Critères communs avant de considérer un registre comme confirmé :

1. Les valeurs observées forment un ensemble fini et stable (par exemple
   uniquement `0` et `1`), sur plusieurs jours.
2. Chaque valeur a été observée au moins une fois **en même temps** que l'état
   réel correspondant, et chaque transition coïncide, à un intervalle de
   lecture près, avec un changement constaté.
3. Au moins une situation où la valeur et la plage horaire divergent a été
   observée, ou bien une observation physique directe la remplace.
4. Aucune valeur inexpliquée n'a été observée. Sinon, le registre reste
   expérimental.

### 3.1 `0x0846` : pompe de bouclage ECS

- **Comparer avec** l'état physique du circulateur de bouclage (bruit,
  vibration, température du retour de bouclage au toucher) et l'entité
  « Bouclage ECS - Plage active ».
- **Moments utiles** : les bords des plages programmées (relevé :
  06:10-08:30 et 21:40-21:50). Observer 5 minutes avant, pendant et 5 minutes
  après.
- **Attendu si l'hypothèse est juste** : deux valeurs, l'une pendant que la
  pompe tourne, l'autre à l'arrêt.
- **Incertitude** : si la valeur suit exactement la plage horaire, cela ne
  prouve pas qu'elle reflète la pompe (elle pourrait refléter la demande de la
  programmation). Seule l'observation physique tranche.
- **Ensuite** : si seules `0`/`1` apparaissent et suivent la pompe, déclarer
  `0x0846` en `binary_sensor` (`device_class: running`) et retirer l'entrée
  TEST.

### 3.2 `0x0845` : pompe de charge ECS

- **Déclencher naturellement un réchauffage** : puiser de l'eau chaude (bain,
  douches) jusqu'à ce que la « Température ECS » (`0x0804`) descende sous la
  consigne (`0x6300`) moins l'hystérésis de la régulation. Ne modifier aucun
  réglage.
- **Observer** pendant tout le cycle : `0x0804` qui remonte, le brûleur
  (`0x0842`), la valeur brute `0x0845`, l'état affiché par la Vitotronic (menu
  d'information ou de diagnostic, selon le niveau d'accès) et, si possible, la
  pompe elle-même.
- **Point discriminant** : la pompe de charge peut continuer à tourner après
  l'arrêt du brûleur (post-circulation). Une valeur qui reste active quelques
  minutes après `0x0842 = 00` distingue `0x0845` d'une simple copie de l'état
  du brûleur.
- **Hors cycle** : la valeur doit rester à l'état de repos plusieurs heures,
  y compris pendant une plage « ECS - Plage active ».
- **Incertitude** : selon l'installation, la charge ECS peut passer par une
  vanne trois voies plutôt qu'une pompe : la valeur peut alors ne jamais
  changer ou avoir un autre sens.

### 3.3 `0x2906` / `0x3906` : circulateurs A1 et M2

- **Comparer avec** l'état des pompes affiché par la Vitotronic pour chaque
  circuit et, si possible, le fonctionnement réel des circulateurs.
- **Situations discriminantes** (sans modifier de réglage) :
  - en saison de chauffe, plage active : pompes normalement en marche ;
  - température extérieure au-dessus de la limite de chauffe, ou été : pompes
    normalement arrêtées alors que la plage horaire peut être active ;
  - mode réduit : selon le paramétrage, la pompe peut continuer à tourner.
- **Incertitude** : un circulateur à vitesse variable peut être codé en
  pourcentage (`0x64` = 100) ou en niveaux. Une valeur autre que 0/1 n'est
  donc pas une erreur de lecture : la noter, et ne pas passer en
  `binary_sensor` dans ce cas.
- Les deux circuits sont indépendants : valider chacun séparément. A1 et M2
  dans le même état ne permettent pas de les distinguer.

### 3.4 `0x0883` : défaut chaudière

- **Ne provoquer aucune panne**, ne couper aucune sonde, n'agir sur aucun
  organe de sécurité.
- **Observation passive** : sur plusieurs semaines, la valeur doit rester à
  une valeur de repos stable (vraisemblablement `0`).
- **Limite** : une valeur constante à `0` est **compatible** avec l'hypothèse
  mais ne la **confirme pas**. Un registre quelconque peut valoir 0.
- **Confirmation possible seulement si** un défaut réel survient de lui-même
  (comparer alors la valeur brute avec le message et le code affichés par la
  Vitotronic, puis son retour au repos après acquittement), ou si une
  documentation fiable du fabricant décrit l'adresse et le codage.
- Le codage peut être un indicateur, un code de défaut ou un champ de bits :
  ne pas le déclarer en `binary_sensor` `problem` sans cette confirmation.
- Après une semaine sans anomalie, le registre peut être retiré du diagnostic
  pour alléger la liaison. Il restera expérimental.

### 3.5 `0x5502` : consigne chaudière calculée

- **Comparer** la valeur brute avec la consigne chaudière affichée par la
  Vitotronic (menu d'information), au même moment.
- **Lire les octets bruts** dans le journal, par exemple `0x5502 <- 4A 02` :
  - int16 = 586 : facteur 0,1 si l'affichage est 58,6 °C ou 58/59 °C ;
  - si seul le premier octet correspond (par exemple `3A 00` et un affichage
    de 58 °C), la grandeur est un uint8 en degrés et le second octet appartient
    à un autre registre ;
  - aucune correspondance : adresse incorrecte.
- **Plusieurs points** : la consigne varie avec la température extérieure
  (courbe de chauffe) et monte pendant une charge ECS. Relever au moins trois
  valeurs nettement différentes, dont une pendant une charge ECS.
- **Plausibilité** (pas une preuve) : en chauffe, la température chaudière
  `0x0802` oscille autour de cette consigne.
- **Ensuite** : seulement si le facteur est confirmé, déclarer le capteur avec
  `unit_of_measurement: "°C"`, `device_class: temperature` et le filtre
  `multiply` établi.

## 4. Charge de la liaison

Lectures par minute (moyenne) :

| Configuration | Lectures/min |
|---|---|
| Production v0.2.1 | ~7,2 |
| Diagnostic, six registres TEST | ~10,9 |
| Diagnostic par groupe de deux | ~7,9 à 9,2 |

Simulation (tests `sim_v022_*`) : chaudière simulée, ordre d'inscription du
firmware, 6 h 30 (démarrage + un rafraîchissement des 28 blocs de
programmation). Référence : 780 lectures du brûleur attendues.

| Resynchro | Production : brûleur, écart max | Diagnostic (6 TEST) : brûleur, écart max | Écart max températures 60 s |
|---|---|---|---|
| 2,0 s | 780, 30 s | 779*, 32 s | 60 s |
| 2,5 s | 780, 36 s | 779*, 38 s | 61 s |
| 3,0 s | 780, 42 s | 779*, 45 s | 70 s |
| 3,5 s | 780, 46 s | 776, 53 s | 81 s |
| 4,0 s | 780, 52 s | 769, 60 s | 93 s |

\* première lecture décalée de 12 s au démarrage (six entrées de plus avant
le brûleur), aucun cycle perdu ensuite.

Dans tous les cas : aucun timeout, aucune requête en double, les 28 blocs de
programmation lus à chaque cycle, aucun registre TEST n'attend plus de deux
intervalles. À 3,5-4 s, les lectures perdues du brûleur se concentrent au
démarrage et pendant les ~5 minutes de rafraîchissement des programmations
(toutes les 6 h) ; hors de ces fenêtres, aucun cycle n'est perdu.

### Diagnostic par groupes (stratégie recommandée)

Le fichier de diagnostic déclare les six registres. La validation recommandée
consiste à n'en activer **qu'un groupe de deux à la fois**, en commentant les
quatre autres entrées TEST :

| Groupe | Registres | Fonction | Conditions d'observation |
|---|---|---|---|
| G1 | `0x0846`, `0x0845` | ECS : bouclage, charge | plages de bouclage, cycle de réchauffage du ballon |
| G2 | `0x2906`, `0x3906` | circulateurs A1, M2 | saison de chauffe, limite de chauffe |
| G3 | `0x0883`, `0x5502` | défaut, consigne chaudière | consigne affichée, plusieurs températures extérieures |

Pourquoi :

- chaque groupe correspond à une campagne d'observation précise (§ 3) ;
- un groupe seul ne coûte aucune lecture au brûleur sur toute la plage de
  resynchro simulée : à 4 s, 780 lectures sur 780, écart maximal 57 s
  (production : 53 s) ;
- l'ordonnanceur n'a pas besoin d'être modifié.

Un groupe validé passe en production (en `binary_sensor` ou `sensor` selon le
résultat) et ses entrées TEST sont retirées avant d'activer le groupe suivant.

## 5. Mémoire (Wemos D1 mini, ESPHome 2026.10.2)

| Firmware | RAM statique | Flash |
|---|---|---|
| Production v0.2.1 (inchangé en v0.2.2) | 33 796 o (41,3 %) | 434 713 o (41,6 %) |
| Diagnostic (six entités TEST) | 34 304 o (+508 o, 41,9 %) | 435 517 o (+804 o, 41,7 %) |

Le surcoût vient uniquement des six entités `sensor` ; le composant n'a pas
changé.
