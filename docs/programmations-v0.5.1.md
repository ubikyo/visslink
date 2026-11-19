# Rafraîchissement des programmations horaires (v0.5.1, étude)

> **v1.0.0 :** document historique. Les options d'autorisation (`enable_*`,
> `allow_write`, `validated_days`), les entités « État modification / commande »
> et les exemples intermédiaires cités ici n'existent plus. État actuel :
> `docs/v1.0.0.md` et `examples/iot-viessmann-v1.0.0.yaml`.


Statut : mesures **simulées** (chaudière simulée, `tests/cpp`). Aucune n'a été
faite sur la chaudière. Validé, non commité.

## Constat

Avec `BLOCK_DELAY_MS = 30 s`, chaque programmation lit ses 7 jours du lundi
au dimanche, un bloc toutes les 30 s au plus vite. Au démarrage, le jour
courant pouvait donc arriver en dernier : jusqu'à 4 à 8 min le dimanche.

## Mesures sur la configuration v0.5 actuelle

Configuration : `examples/iot-viessmann.yaml` v0.5, soit :

- 4 températures à 60 s, 2 compteurs à 1 h ;
- brûleur et 4 Éco/Party à 30 s (valeur de la v0.5, 60 s depuis la v0.5.1) ;
- 5 consignes et 2 modes à 5 min ;
- 4 programmations à 6 h ;
- horloge à 10 min.

| Resynchro | Jour courant (dimanche), v0.5 | Jour courant, v0.5.1 | 28 blocs (délai 30 s) |
|---|---|---|---|
| 2 s | 238 s | **47 s** | 238 s |
| 3 s | 364 s | **69 s** | 364 s |
| 4 s | 471 s | **91 s** | 471 s |

- La v0.5.1 ne fait **aucune lecture en plus** : même nombre de requêtes et
  même cadence du brûleur, à une lecture près.
- Les 47 s restantes viennent de l'étalement du démarrage (2 s par registre
  déclaré) et de la liaison, pas de la pause entre blocs.

### Effet de `BLOCK_DELAY_MS` (temps pour les 28 blocs)

| Resynchro | 30 s | 20 s | 15 s | 10 s | Écart max. du brûleur (30 → 10 s) |
|---|---|---|---|---|---|
| 2 s | 238 s | 188 s | 167 s | 153 s | 30,7 → 36,8 s |
| 3 s | 364 s | 325 s | 297 s | 276 s | 45,7 → 51,8 s |
| 4 s | 471 s | 435 s | 411 s | 386 s | liaison déjà saturée |

- **Le goulot est la liaison KW, pas la pause.** À 3 s, la configuration
  occupe déjà presque toute la liaison. Diviser la pause par 3 ne fait gagner
  que 24 % sur la semaine, au prix d'un écart du brûleur plus grand.
- **20 s est neutre à 2 et 3 s** : mêmes écarts pour le brûleur et Éco/Party,
  780 lectures du brûleur sur 780. Le gain sur la semaine est de 11 à 21 %.
- **À 4 s, la configuration v0.5 est déjà surchargée**, quelle que soit la
  pause : le brûleur fait environ 700 lectures sur 780. Ce n'est pas une
  question de programmations : c'est Éco/Party à 30 s (cf. `registres.md`
  § 6). La pause n'y change rien.

## Ce qui est implémenté (non commité)

1. **Jour courant d'abord** : chaque cycle d'une programmation commence par
   le jour donné par l'horloge `time`, puis fait le tour de la semaine.
   - Sans heure valide, l'ordre reste lundi → dimanche.
   - Si l'heure arrive après le début du premier cycle, le jour courant est
     relu hors cycle, une seule fois.
   - Ce jour est publié dès réception de son bloc : capteur du jour, plage
     active, et résumé de la semaine une fois les 7 jours tentés.
2. **Relecture ponctuelle d'un bloc** :
   `VissLinkComponent::request_block_refresh(adresse)`.
   - Un seul bloc est relu, en priorité, et publié comme une lecture
     normale. Le cycle hebdomadaire n'est ni avancé ni recommencé.
   - Elle n'est **jamais** servie pendant une relecture de vérification
     d'écriture, qui reste prioritaire.
   - Elle est refusée hors programmation déclarée ou si une relecture est
     déjà en attente pour ce registre.
   - Mesuré : publication en 0,3 à 7,5 s selon la phase de la liaison, au
     lieu d'attendre jusqu'à 6 h.
3. **Jour illisible** : il n'est pas relu en boucle. Une seule demande de
   relecture par jour courant, puis il attend le cycle suivant.
4. **`BLOCK_DELAY_MS` inchangé (30 s)**. La pause est réglable dans
   l'ordonnanceur (`set_block_delay`) pour les simulations, mais n'est pas
   exposée en YAML.

Inchangé :

- les temporisations KW (`sync_timeout`, `response_timeout`, `RETRY_DELAY_MS`) ;
- les écritures de calendrier (`enable_schedule_writes: false`) ;
- `validated_days: []`.

## Mesures complémentaires

| Resynchro | Heure valide à 120 s → jour publié | Relecture ponctuelle | Rafraîchissement à 6 h (28 blocs) |
|---|---|---|---|
| 2 s | +6,5 s | 1,4 s | 199 s |
| 3 s | +13,0 s | 0,3 s | 289 s |
| 4 s | +19,6 s | 7,5 s | 409 s |

## Décisions (v0.5.1)

- `BLOCK_DELAY_MS` reste à 30 s, sans paramètre YAML.
- La lecture prioritaire du jour courant, `request_block_refresh` et
  `invalid_values` (désactivé par défaut) sont conservés.
- Le bouclage à la demande utilise la relecture ciblée pour actualiser
  l'affichage après une modification confirmée
  ([`bouclage-ecs-v0.5.md`](bouclage-ecs-v0.5.md) § 5, « Actualisation de
  l'affichage »).
- Éco/Party à 60 s dans les deux exemples (production et essai en lecture
  seule). Essai : ~13 requêtes/min au lieu de ~17 ; le brûleur est lu 780
  fois sur 780 à 2 et 3 s de resynchro, et 719 fois (au lieu de 593) à 4 s.
  Production : voir `registres.md` § 6 (763/780 à 4 s au lieu de 700/780).

Reproduire : `VL_REPORT=1 ./tests/cpp/build/visslink_tests sim_v051_report`.
