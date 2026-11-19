# Diagnostics v0.8.1 : cycles du brûleur et états initiaux

> **v1.0.0 :** document historique. Les options d'autorisation (`enable_*`,
> `allow_write`, `validated_days`), les entités « État modification / commande »
> et les exemples intermédiaires cités ici n'existent plus. État actuel :
> `docs/v1.0.0.md` et `examples/iot-viessmann-v1.0.0.yaml`.


Exemple : `examples/iot-viessmann-diagnostics-v0.8.1.yaml`. Il reprend
l'exemple v0.8 (programmations) sans le modifier par ailleurs.

## 1. Cycles du brûleur (`visslink_burner`)

### Aucune lecture Optolink en plus

Le composant ne lit rien et n'écrit rien. Une fois par seconde, il regarde
si les entités existantes ont fait une nouvelle lecture réussie. Il utilise
la date de cette lecture et sa valeur.

| Registre | Entité | Intervalle (exemple) | Utilisation |
|---|---|---|---|
| `0x0842` | `bruleur_actif` (binary_sensor) | 30 s | marche/arrêt, durées |
| `0x088A` | `demarrages_bruleur` (uint32) | 1 h | démarrages |
| `0x08A7` | `fonctionnement_bruleur` (uint32, secondes) | 1 h | durée moyenne |
| `0x0802` | `temperature_chaudiere` | 60 s | températures au démarrage et à l'arrêt |

La validation refuse une autre adresse, un autre type (les compteurs doivent
être en `uint32`) ou une entité qui n'est pas `visslink`. Les compteurs sont
utilisés en valeur brute exacte, avant les filtres du YAML.

### Valeurs exactes (compteurs de la chaudière)

| Entité | Calcul |
|---|---|
| Démarrages sur 24 h | différence du compteur `0x088A` entre deux lectures séparées d'environ 24 h |
| Démarrages par heure | même différence, divisée par la durée réelle entre les deux lectures |
| Durée moyenne de fonctionnement | différence de `0x08A7` (s) divisée par la différence de `0x088A` |

- **Fenêtre :** la fenêtre de 24 h va de la lecture la plus récente ayant au
  moins 23 h 55 min jusqu'à la dernière lecture. Avec un compteur lu toutes
  les heures, elle dure entre 23 h 55 min et environ 25 h. Rien n'est
  extrapolé.
- **Disponibilité :**
  - « Démarrages sur 24 h » est inconnu pendant les 24 premières heures après
    un démarrage de l'ESP, car l'historique est en RAM.
  - « Démarrages par heure » et « Durée moyenne » sont publiés dès qu'une
    heure de lectures est disponible, sur la fenêtre disponible (24 h au
    plus).
  - « Brûleur - Précision des cycles » indique la fenêtre en cours.
- **Inconnu :**
  - si la dernière lecture du compteur date de plus de 3 intervalles + 30 s ;
  - si les lectures manquent plus de 26 h ;
  - si aucun démarrage n'a eu lieu dans la fenêtre (pour la durée moyenne,
    sans division par zéro) ;
  - si les fenêtres des deux compteurs diffèrent de plus de 10 %.
- **Compteur qui recule** (remise à zéro ou lecture aberrante) : l'historique
  repart de ce point, avec un avertissement dans le journal.
- **Durée moyenne :** les deux compteurs sont lus séparément, à quelques
  secondes d'intervalle. La moyenne est donc exacte à ce décalage près.

### Valeurs estimées (lecture du brûleur toutes les 30 s)

| Entité | Calcul |
|---|---|
| Durée du dernier cycle | du démarrage estimé à l'arrêt estimé |
| Durée du dernier arrêt | de l'arrêt estimé au démarrage estimé |
| Température au démarrage / à l'arrêt | lecture de `0x0802` la plus proche de l'instant estimé |

- **Datation d'un changement d'état :** il a lieu entre la dernière lecture
  de l'ancien état et la première lecture du nouveau. Il est daté au milieu
  de cet intervalle, donc à ± 15 s avec une lecture toutes les 30 s. Une
  durée cumule l'incertitude de ses deux bornes, soit ± 30 s en général.
  L'incertitude du dernier cycle figure dans « Précision des cycles » et
  dans le journal (`Burner cycle: 600 s +/- 30 s`).
- **Lecture manquée :**
  - une lecture manquée isolée (60 s entre deux lectures) reste acceptée,
    avec une incertitude plus grande ;
  - au-delà de 3 intervalles + 30 s sans lecture, ou après une lecture
    inconnue, la borne est inconnue et la durée correspondante est publiée
    « inconnue ».
- **Cycle court :** un cycle plus court que l'intervalle de lecture peut
  passer inaperçu. Le compteur de démarrages, lui, le compte : un écart entre
  « Démarrages par heure » et les cycles vus signale des cycles courts.
- **Température :** la lecture d'avant ou la première lecture d'après le
  changement d'état, la plus proche de l'instant estimé. Elle est publiée
  inconnue si aucune n'est à moins de 3 intervalles + 30 s, soit 3 min 30 s
  pour une lecture toutes les 60 s. Avec une lecture toutes les 60 s, elle
  peut dater jusqu'à environ 45 s de plus ou de moins que le changement réel.
- **Au démarrage de l'ESP :** la première phase n'a pas de début connu. Le
  premier cycle et le premier arrêt sont donc publiés « inconnus ».

### Charge

- **Lectures Optolink :** aucune en plus.
- **RAM :** environ 1 Ko, dont 2 historiques de 56 lectures au plus. Les
  lectures du compteur sont gardées toutes les 30 min au plus, sur un peu
  plus de 24 h.
- **Mesure de compilation de l'exemple pour D1 mini :** RAM 55,8 %, flash
  52,0 %.

## 2. Diagnostics de fonctionnement conservés

- **État du bouclage, de l'éditeur des programmations et horloge de la
  Vitotronic :** inchangés.
- **Consigne chaudière calculée (`0x5502`) :** activée en diagnostic, une
  lecture toutes les 2 min, en int16 × 0,1 °C (cf. `docs/registres.md`
  § 3.5).
- **Réseau et uptime :** `wifi_signal`, `wifi_info` (adresse IP, réseau) et
  `uptime`. Ce sont des plateformes ESPHome, sans lecture Optolink.

## 3. « État modification » des consignes

- **Avant :** au démarrage, ces capteurs n'avaient pas de valeur, et Home
  Assistant affichait « Inconnu », comme pour un défaut.
- **Depuis la v0.8.1 :** ils affichent « Aucune modification effectuée »
  jusqu'à la première demande, ou jusqu'à une modification détectée au
  panneau.
- **Inchangé :** les écritures et les autres messages.

## 4. Capteur `0xA38F` (puissance brute)

- **Constat :** il renvoie toujours `FF FF` sur cette installation.
- **État du dépôt :** il n'est présent dans aucun exemple. Il n'y avait donc
  rien à y retirer.
- **YAML personnel :** s'il s'y trouve, le retirer à la main. Ce fichier n'est
  pas modifié par VissLink.
