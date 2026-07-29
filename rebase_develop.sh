#!/usr/bin/env bash
#
# rebase_develop.sh — ребейз ДЕРЕВА веток, выросших из <root> (по умолчанию develop),
# с сохранением исходной структуры и БЕЗ дублирования коммитов.
#
# Что умеет:
#   • строит дерево всех локальных веток-потомков <root> (рекурсивно);
#   • обходит ветки в топологическом порядке (родитель раньше ребёнка);
#   • каждую ветку перебазирует на НОВЫЙ tip её настоящего родителя, а не на root —
#     поэтому общие коммиты не дублируются, а дерево сохраняется;
#   • внутренние точки ветвления (когда ветка откололась от внутреннего коммита
#     чужой линии, а не от tip'а) переносится по patch-id на соответствующий
#     новый коммит (использует git patch-id --stable);
#   • по умолчанию только печатает план (dry-run); для выполнения нужен --apply;
#   • создаёт резервные ref'ы refs/backups/<ts>/<branch>.
#
# Примеры:
#   ./rebase_develop.sh                     # план: rebase дерева develop -> origin/develop
#   ./rebase_develop.sh --apply             # выполнить (onto=origin/develop)
#   ./rebase_develop.sh --onto master --apply
#   ./rebase_develop.sh --fetch --apply     # git fetch origin перед работой
#
# Откатить ветку:
#   git update-ref refs/heads/<branch> refs/backups/<ts>/<branch>

set -euo pipefail

# --- требования к bash (нужны ассоциативные массивы, bash >= 4) ---
if [[ "${BASH_VERSINFO[0]:-0}" -lt 4 ]]; then
    echo "Требуется bash >= 4 (найден ${BASH_VERSION}). На macOS используйте brew bash." >&2
    exit 1
fi

# --------------------------------------------------------------------------- #
#  Аргументы
# --------------------------------------------------------------------------- #
ROOT="develop"
ONTO=""
APPLY=false
NO_BACKUP=false
FORCE=false
DO_FETCH=false

usage() {
    sed -n '3,30p' "$0"
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --root)        ROOT="$2"; shift 2 ;;
        --onto)        ONTO="$2"; shift 2 ;;
        --apply)       APPLY=true; shift ;;
        --no-backup)   NO_BACKUP=true; shift ;;
        --force)       FORCE=true; shift ;;
        --fetch)       DO_FETCH=true; shift ;;
        -h|--help)     usage 0 ;;
        *) echo "Неизвестный аргумент: $1" >&2; usage 1 ;;
    esac
done

# --------------------------------------------------------------------------- #
#  Вспомогательные функции
# --------------------------------------------------------------------------- #
tip()       { git rev-parse --short=10 "$1"; }
tip_full()  { git rev-parse "$1"; }

is_anc() { # is_anc <maybe-ancestor> <descendant>
    git merge-base --is-ancestor "$1" "$2"
}

patch_id_of() { # patch_id_of <commit>
    local pid
    pid=$(git diff "$1^" "$1" 2>/dev/null | git patch-id --stable | awk '{print $1}')
    echo "$pid"
}

# --------------------------------------------------------------------------- #
#  Предпроверки
# --------------------------------------------------------------------------- #
if ! $FORCE; then
    if [[ -n "$(git status --porcelain)" ]]; then
        echo "Working tree не чистый. Закоммитьте/спрячьте изменения или --force." >&2
        git status --short >&2
        exit 1
    fi
fi

git rev-parse --verify --quiet "$ROOT" >/dev/null 2>&1 || {
    echo "Корневая ветка не найдена: $ROOT" >&2; exit 1; }

CURRENT_BRANCH="$(git branch --show-current || true)"

if $DO_FETCH; then
    echo "→ git fetch origin"
    git fetch origin --tags --prune
fi

# onto по умолчанию = origin/<root>
[[ -z "$ONTO" ]] && ONTO="origin/$ROOT"
git rev-parse --verify --quiet "$ONTO" >/dev/null 2>&1 || {
    echo "Цель ребейза не найдена: $ONTO" >&2; exit 1; }

ROOT_FULL="$(tip_full "$ROOT")"
ONTO_FULL="$(tip_full "$ONTO")"

# Исходные tips всех веток дерева. НЕ ИЗМЕНЯЮТСЯ во время ребейза.
# Нужны, чтобы отличать "ответвление от tip'а родителя" (fork == original_tip[parent])
# от "внутренний fork" (fork == внутренний коммит линии родителя) — сравнивать надо
# с ИСХОДНЫМ tip'ом родителя, а не с уже перебазированным.
declare -A ORIG_TIP

# --------------------------------------------------------------------------- #
#  1. Собираем множество веток дерева (включая root)
# --------------------------------------------------------------------------- #
TREE=()
while read -r b; do
    [[ "$b" == "main" || "$b" == "master" ]] && continue
    if [[ "$b" == "$ROOT" ]]; then
        TREE+=("$b")
    elif is_anc "$ROOT_FULL" "$(tip_full "$b")"; then
        TREE+=("$b")
    fi
done < <(git for-each-ref --format='%(refname:short)' refs/heads)

if [[ ${#TREE[@]} -eq 0 ]]; then
    echo "Веток-потомков $ROOT не найдено." >&2; exit 0
fi

# --------------------------------------------------------------------------- #
#  2. Для каждой ветки находим ближайшего предка-ветку (deepest ancestor tip)
#     Среди кандидатов-предков выбираем того, который сам является потомком
#     всех остальных кандидатов (= нижний конец цепочки предков).
# --------------------------------------------------------------------------- #
declare -A PARENT
parent_of() {
    local b="$1" bt cc cc2
    bt="$(tip_full "$b")"
    local cands=()
    for cc in "${TREE[@]}"; do
        [[ "$cc" == "$b" ]] && continue
        is_anc "$(tip_full "$cc")" "$bt" && cands+=("$cc")
    done
    if [[ ${#cands[@]} -eq 0 ]]; then
        echo ""          # корень дерева (нет предка-ветки)
        return
    fi
    local deepest=""
    for cc in "${cands[@]}"; do
        local ok=1
        for cc2 in "${cands[@]}"; do
            [[ "$cc" == "$cc2" ]] && continue
            if ! is_anc "$(tip_full "$cc2")" "$(tip_full "$cc")"; then ok=0; break; fi
        done
        if [[ $ok -eq 1 ]]; then deepest="$cc"; break; fi
    done
    [[ -z "$deepest" ]] && deepest="${cands[0]}"   # недеревянный DAG — берём первого
    echo "$deepest"
}

for b in "${TREE[@]}"; do
    PARENT["$b"]="$(parent_of "$b")"
    ORIG_TIP["$b"]="$(tip_full "$b")"
done

# --------------------------------------------------------------------------- #
#  3. Топологическая сортировка (итеративно): родитель раньше ребёнка
# --------------------------------------------------------------------------- #
ORDER=()
_in_order=()
_in_order_has() { local x="$1"; for o in "${_in_order[@]}"; do [[ "$o" == "$x" ]] && return 0; done; return 1; }

remaining=("${TREE[@]}")
while [[ ${#remaining[@]} -gt 0 ]]; do
    progress=0
    nextrem=()
    for b in "${remaining[@]}"; do
        p="${PARENT[$b]}"
        if [[ -z "$p" ]] || _in_order_has "$p"; then
            ORDER+=("$b"); _in_order+=("$b"); progress=1
        else
            nextrem+=("$b")
        fi
    done
    [[ $progress -eq 0 ]] && { echo "Обнаружен цикл в структуре дерева." >&2; exit 1; }
    [[ ${#nextrem[@]} -eq 0 ]] && break
    remaining=("${nextrem[@]}")
done

# --------------------------------------------------------------------------- #
#  4. Печать плана
# --------------------------------------------------------------------------- #
echo "=================================================================="
echo " Корень дерева : $ROOT ($(tip "$ROOT"))"
echo " Ребейз на     : $ONTO ($(tip "$ONTO"))"
echo " Режим         : $(${APPLY} && echo 'ПРИМЕНЕНИЕ' || echo 'ПЛАН (dry-run)')"
echo " Веток в дереве: ${#TREE[@]}"
echo "================================================================="
for b in "${ORDER[@]}"; do
    p="${PARENT[$b]}"
    if [[ -z "$p" ]]; then
        echo " • ${b}  ROOT  --rebase--> $ONTO"
    else
        fork="$(git merge-base "$(tip_full "$p")" "$(tip_full "$b")")"
        if [[ "$fork" == "$(tip_full "$p")" ]]; then
            echo " • ${b}  parent=${p}  fork=$(tip "$fork")  [tip]"
        else
            echo " • ${b}  parent=${p}  fork=$(tip "$fork")  [внутренний -> по patch-id]"
        fi
    fi
done
echo "================================================================="

if ! $APPLY; then
    echo "Это только план. Добавьте --apply для выполнения."
    exit 0
fi

# --------------------------------------------------------------------------- #
#  5. Бэкапы
# --------------------------------------------------------------------------- #
TS="$(date +%Y%m%d-%H%M%S)"
if ! $NO_BACKUP; then
    for b in "${TREE[@]}"; do
        git update-ref "refs/backups/$TS/$b" "$(tip_full "$b")"
    done
    echo "Резерв: refs/backups/$TS/<branch>"
fi

# --------------------------------------------------------------------------- #
#  6. Выполнение ребейзов
#     PID2NEW[patchid] = new_commit — глобальная карта всех перебазированных
#     коммитов (по топологическому порядку предки уже в карте к моменту ребёнка).
# --------------------------------------------------------------------------- #
declare -A NEWTIP
declare -A PID2NEW

# внести в карту patch-id новые коммиты диапазона base..newtip ветки owner
fill_pid() { # fill_pid <base> <newtip>
    local base="$1" newtip="$2" nc pid
    while read -r nc; do
        [[ -z "$nc" ]] && continue
        pid="$(patch_id_of "$nc")"
        [[ -n "$pid" ]] && PID2NEW["$pid"]="$nc"
    done < <(git rev-list --reverse "$base..$newtip")
}

echo ""
for b in "${ORDER[@]}"; do
    p="${PARENT[$b]}"

    if [[ -z "$p" ]]; then
        # корень дерева -> onto
        newbase="$ONTO_FULL"
        oldbase="$(git merge-base "$ROOT_FULL" "$ONTO_FULL")"
    else
        ptip="${NEWTIP[$p]:-$(tip_full "$p")}"   # новый tip родителя после его ребейза
        orig_ptip="${ORIG_TIP[$p]}"               # ИСХОДНЫЙ tip родителя ДО ребейза
        # fork считаем относительно ИСХОДНОГО tip'а родителя: ребёнок откололся
        # от старой версии линии, а не от уже перебазированной.
        fork="$(git merge-base "$orig_ptip" "${ORIG_TIP[$b]}")"
        if [[ "$fork" == "$orig_ptip" ]]; then
            # чистое ответвление от tip'а родителя -> берём НОВЫЙ tip родителя
            newbase="$ptip"
            oldbase="$fork"
        else
            # внутренний fork (от внутреннего коммита линии родителя):
            # переносим точку крепления на новый SHA по patch-id
            pid="$(patch_id_of "$fork")"
            newbase="${PID2NEW[$pid]:-}"
            if [[ -z "$newbase" ]]; then
                # коммит не пересоздавался (например, лежит ниже точки ребейза) — как есть
                newbase="$fork"
            fi
            oldbase="$fork"
        fi
    fi

    echo "→ rebase $b  (--onto $(tip "$newbase")  $(tip "$oldbase"))"
    git checkout "$b" >/dev/null 2>&1

    if git rebase --onto "$newbase" "$oldbase" "$b"; then
        NEWTIP["$b"]="$(tip_full "$b")"
        # новая линия ветки = её собственные новые коммиты (oldbase..newtip в новых SHA)
        fill_pid "$newbase" "${NEWTIP[$b]}"
        if [[ "$b" == "$ROOT" ]]; then
            ROOT_FULL="$(tip_full "$b")"   # корень мог сместиться
        fi
        echo "  ✓ $b  ($(tip "${ORIG_TIP[$b]}") -> $(tip "$b"))"
    else
        echo "❌ Конфликт при ребейзе $b." >&2
        echo "   Разрешите вручную: git rebase --continue  |  git rebase --abort" >&2
        echo "   Резерв: refs/backups/$TS/$b" >&2
        git rebase --abort 2>/dev/null || true
        exit 1
    fi
done

# возврат на исходную ветку
if [[ -n "$CURRENT_BRANCH" && "$CURRENT_BRANCH" != "HEAD" ]]; then
    git checkout "$CURRENT_BRANCH" >/dev/null 2>&1 || true
fi

echo ""
echo "Готово. Старые tips сохранены в refs/backups/$TS/"
echo "Откат: git update-ref refs/heads/<branch> refs/backups/$TS/<branch>"