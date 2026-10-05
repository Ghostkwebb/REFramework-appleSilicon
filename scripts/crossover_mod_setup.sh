#!/usr/bin/env bash
#
# crossover_mod_setup.sh
# macOS helper for REFramework on CrossOver
#
# 1. Finds Capcom RE Engine games in CrossOver bottles
# 2. Configures dinput8 (native, builtin) override in wine registry
# 3. Creates convenient Finder shortcuts under ~/CapcomMods/<Game>
#

set -euo pipefail

BOTTLE_DIRS=(
    "$HOME/Library/Application Support/CrossOver/Bottles"
    "$HOME/Library/Containers/com.isaacmarovitz.Whisky/Data/Library/Application Support/Whisky/Bottles"
)
MODS_DIR="$HOME/CapcomMods"

echo "============================================="
echo "  REFramework macOS / CrossOver / Whisky Helper "
echo "============================================="

mkdir -p "$MODS_DIR"

declare -a CAPCOM_EXES=(
    "MonsterHunterWilds.exe"
    "MonsterHunterRise.exe"
    "re4.exe"
    "re8.exe"
    "re2.exe"
    "re3.exe"
    "re7.exe"
    "devilmaycry5.exe"
    "StreetFighter6.exe"
    "DD2.exe"
    "KunitsuGami.exe"
    "DRDR.exe"
    "Pragmata.exe"
    "pragmata.exe"
)

FOUND_GAMES=0

for bottle_dir in "${BOTTLE_DIRS[@]}"; do
    if [ ! -d "$bottle_dir" ]; then continue; fi
    for bottle in "$bottle_dir"/*; do
        if [ ! -d "$bottle" ]; then continue; fi
        bottle_name=$(basename "$bottle")
    
    for exe in "${CAPCOM_EXES[@]}"; do
        game_path=$(find "$bottle" -name "$exe" -maxdepth 8 -type f 2>/dev/null | head -n 1 || true)
        if [ -n "$game_path" ]; then
            FOUND_GAMES=$((FOUND_GAMES + 1))
            game_dir=$(dirname "$game_path")
            game_name=$(basename "$exe" .exe)
            
            echo ""
            echo "[+] Found $exe in bottle '$bottle_name':"
            echo "    Location: $game_dir"

            # 1. Check/Configure dinput8 override in user.reg
            user_reg="$bottle/user.reg"
            if [ -f "$user_reg" ]; then
                if grep -qi '"dinput8"="native,builtin"' "$user_reg"; then
                    echo "    [ok] dinput8 override already set to (native, builtin)"
                else
                    echo "    [*] Setting dinput8 override in $user_reg..."
                    # Check if [Software\\Wine\\DllOverrides] section exists
                    if grep -qi '\[Software\\\\Wine\\\\DllOverrides\]' "$user_reg"; then
                        sed -i '' '/\[Software\\\\Wine\\\\DllOverrides\]/a\
"dinput8"="native,builtin"
' "$user_reg"
                    else
                        cat << 'EOF' >> "$user_reg"

[Software\\Wine\\DllOverrides]
"dinput8"="native,builtin"
EOF
                    fi
                    echo "    [+] dinput8 override successfully added!"
                fi
            fi

            # 2. Setup convenient symlink in ~/CapcomMods/<GameName>
            target_link="$MODS_DIR/$game_name ($bottle_name)"
            mkdir -p "$game_dir/natives"
            mkdir -p "$game_dir/pak_mods"
            mkdir -p "$game_dir/reframework/autorun"
            mkdir -p "$game_dir/reframework/plugins"
            
            rm -f "$target_link"
            ln -s "$game_dir" "$target_link"
            echo "    [+] Created Finder symlink: $target_link"
        fi
    done
done
done

echo ""
if [ "$FOUND_GAMES" -eq 0 ]; then
    echo "[-] No Capcom RE Engine games found in CrossOver bottles."
    echo "    Make sure your bottle has the game installed via Steam."
else
    echo "[✓] Done! Found $FOUND_GAMES game(s)."
    echo "    Place dinput8.dll into each game folder."
    echo "    Quick access to game folders created in: $MODS_DIR"
fi
