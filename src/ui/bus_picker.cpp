#include "ui/bus_picker.hpp"

#include "omsi/logger.hpp"
#include "omsi/utf8_win32.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>

#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace omsi::ui {
namespace {

constexpr int IDC_TREE = 2001;
constexpr int IDC_OK = 2002;
constexpr int IDC_CANCEL = 2003;
constexpr int IDC_FILTER = 2004;
constexpr int IDC_STATUS = 2005;

struct TreeItemRec {
    int label_idx = -1;
    int leaf_idx = -1;
};

struct PickerState {
    std::vector<omsi::bus_catalog::BusEntry> entries;
    std::vector<omsi::bus_catalog::BusEntry> filtered;
    std::vector<std::wstring> tree_labels;
    std::vector<TreeItemRec> tree_items;
    std::filesystem::path selected;
    bool done = false;
    bool ok = false;
    HWND tree = nullptr;
    HWND filter = nullptr;
    HWND status = nullptr;
    std::wstring filter_text;
};

std::wstring to_wide(const std::string& s) { return omsi::utf8_to_wide(s); }

std::string ascii_lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool entry_matches(const omsi::bus_catalog::BusEntry& e, const std::string& q) {
    if (q.empty()) return true;
    const std::string hay = ascii_lower(e.friendly.category + " " + e.friendly.model + " " + e.friendly.variant +
                                        " " + e.folder_name + " " + e.file_name);
    return hay.find(q) != std::string::npos;
}

int push_label(PickerState* st, std::wstring text) {
    st->tree_labels.push_back(std::move(text));
    return static_cast<int>(st->tree_labels.size()) - 1;
}

LPARAM push_tree_item(PickerState* st, int label_idx, int leaf_idx) {
    st->tree_items.push_back({label_idx, leaf_idx});
    return static_cast<LPARAM>(st->tree_items.size() - 1);
}

HTREEITEM insert_node(PickerState* st, HTREEITEM parent, int label_idx, int leaf_idx) {
    TVINSERTSTRUCTW tvi{};
    tvi.hParent = parent;
    tvi.hInsertAfter = TVI_LAST;
    tvi.item.mask = TVIF_TEXT | TVIF_PARAM;
    tvi.item.pszText = reinterpret_cast<LPWSTR>(LPSTR_TEXTCALLBACK);
    tvi.item.lParam = push_tree_item(st, label_idx, leaf_idx);
    return TreeView_InsertItem(st->tree, &tvi);
}

void refill_tree(PickerState* st) {
    if (!st || !st->tree) return;
    TreeView_DeleteAllItems(st->tree);
    st->tree_labels.clear();
    st->tree_items.clear();
    st->filtered.clear();
    st->filtered.reserve(st->entries.size());

    const std::string q = ascii_lower(omsi::wide_to_utf8(st->filter_text));
    for (const auto& e : st->entries) {
        if (entry_matches(e, q)) st->filtered.push_back(e);
    }

    std::map<std::string, std::map<std::string, std::vector<std::size_t>>> tree;
    for (std::size_t i = 0; i < st->filtered.size(); ++i) {
        const auto& e = st->filtered[i];
        tree[e.friendly.category][e.friendly.model].push_back(i);
    }

    for (const auto& [cat, models] : tree) {
        std::size_t cat_count = 0;
        for (const auto& m : models) cat_count += m.second.size();
        const int cat_label = push_label(st, to_wide(cat + "  (" + std::to_string(cat_count) + ")"));
        const HTREEITEM h_cat = insert_node(st, TVI_ROOT, cat_label, -1);

        for (const auto& [model, idxs] : models) {
            std::wstring model_w = to_wide(model);
            if (idxs.size() > 1) model_w += L"  (" + std::to_wstring(idxs.size()) + L")";
            const int model_label = push_label(st, std::move(model_w));
            const HTREEITEM h_model = insert_node(st, h_cat, model_label, -1);

            for (const std::size_t idx : idxs) {
                const auto& e = st->filtered[idx];
                const int leaf_label = push_label(st, to_wide(e.list_label));
                insert_node(st, h_model, leaf_label, static_cast<int>(idx));
            }
        }
    }

    for (HTREEITEM h = TreeView_GetNextItem(st->tree, TVI_ROOT, TVGN_CHILD); h;
         h = TreeView_GetNextItem(st->tree, h, TVGN_NEXT))
        TreeView_Expand(st->tree, h, TVE_COLLAPSE);

    if (st->status) {
        wchar_t buf[128]{};
        swprintf_s(buf, L"%zu buses  |  filtro: %zu", st->entries.size(), st->filtered.size());
        SetWindowTextW(st->status, buf);
    }
}

bool try_accept_selection(PickerState* st) {
    if (!st || !st->tree) return false;
    const HTREEITEM sel = TreeView_GetSelection(st->tree);
    if (!sel) return false;
    TVITEMW item{};
    item.mask = TVIF_PARAM;
    item.hItem = sel;
    if (!TreeView_GetItem(st->tree, &item)) return false;
    const auto rec_idx = static_cast<std::size_t>(item.lParam);
    if (rec_idx >= st->tree_items.size()) return false;
    const int leaf = st->tree_items[rec_idx].leaf_idx;
    if (leaf < 0) return false;
    const auto idx = static_cast<std::size_t>(leaf);
    if (idx >= st->filtered.size()) return false;
    st->selected = st->filtered[idx].bus_path;
    st->ok = true;
    st->done = true;
    return true;
}

LRESULT CALLBACK BusPickerWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<PickerState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        st = static_cast<PickerState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

        CreateWindowExW(0, L"STATIC", L"Filtrar:", WS_CHILD | WS_VISIBLE, 12, 12, 60, 18, hwnd, nullptr, nullptr,
                        nullptr);
        st->filter = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                     WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP, 72, 10, 420, 22, hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_FILTER)), nullptr, nullptr);
        st->tree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASLINES | TVS_LINESATROOT |
                                       TVS_HASBUTTONS | TVS_SHOWSELALWAYS,
                                   12, 40, 560, 360, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TREE)),
                                   nullptr, nullptr);
        st->status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 12, 410, 360, 18, hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS)), nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Abrir", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP, 380, 406, 90,
                        28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_OK)), nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Cancelar", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 480, 406, 90, 28, hwnd,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CANCEL)), nullptr, nullptr);

        refill_tree(st);
        return 0;
    }
    case WM_COMMAND: {
        if (!st) break;
        const int id = LOWORD(wp);
        const int code = HIWORD(wp);
        if (id == IDC_FILTER && code == EN_CHANGE) {
            wchar_t buf[256]{};
            GetWindowTextW(st->filter, buf, 256);
            st->filter_text = buf;
            refill_tree(st);
            return 0;
        }
        if (id == IDC_OK) {
            if (try_accept_selection(st)) DestroyWindow(hwnd);
            else
                MessageBoxW(hwnd, L"Elegí un bus (hoja del árbol: variante / carpeta).", L"Bus",
                            MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (id == IDC_CANCEL) {
            st->ok = false;
            st->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    case WM_NOTIFY: {
        if (!st) break;
        const auto* hdr = reinterpret_cast<LPNMHDR>(lp);
        if (hdr->idFrom == IDC_TREE) {
            if (hdr->code == TVN_GETDISPINFOW) {
                auto* di = reinterpret_cast<NMTVDISPINFOW*>(lp);
                const auto rec_idx = static_cast<std::size_t>(di->item.lParam);
                if (rec_idx < st->tree_items.size()) {
                    const int li = st->tree_items[rec_idx].label_idx;
                    if (li >= 0 && li < static_cast<int>(st->tree_labels.size())) {
                        wcsncpy_s(di->item.pszText, di->item.cchTextMax, st->tree_labels[li].c_str(), _TRUNCATE);
                        di->item.mask |= TVIF_DI_SETITEM;
                    }
                }
                return 0;
            }
            if (hdr->code == NM_DBLCLK) {
                if (try_accept_selection(st)) DestroyWindow(hwnd);
                return 0;
            }
        }
        break;
    }
    case WM_CLOSE:
        if (st) {
            st->ok = false;
            st->done = true;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

std::optional<std::filesystem::path> show_bus_picker(const std::filesystem::path& omsi_root) {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    PickerState st{};
    st.entries = omsi::bus_catalog::scan_vehicles(omsi_root);
    if (st.entries.empty()) {
        MessageBoxW(nullptr, L"No se encontraron .bus en Vehicles\\{carpeta}\\*.bus", L"Bus", MB_OK | MB_ICONWARNING);
        return std::nullopt;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = BusPickerWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"OmsiF4BusPicker";
    RegisterClassExW(&wc);

    const HWND hwnd =
        CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"Elegir vehículo (.bus)",
                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, 600,
                        480, nullptr, nullptr, wc.hInstance, &st);
    if (!hwnd) {
        omsi::log::error("bus picker CreateWindowEx failed");
        return std::nullopt;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg{};
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (!st.ok || st.selected.empty()) return std::nullopt;
    omsi::log::info("bus picker selected: " + omsi::path_text(st.selected));
    return st.selected;
}

} // namespace omsi::ui
