// Vue.js refactored version of printsend.js
const { createApp, unref } = Vue;
const { ElInput, ElButton, ElPopover, ElLoading } = ElementPlus;

const PrintSendApp = {
    data() {
        return {
            // Print  data

            printInfo: {
                modelName: '',
                printTime: '00:00:00',
                totalWeight: 0,
                layerCount: 0,
                thumbnail: '',
                timeLapse: false,
                heatedBedLeveling: false,
                uploadAndPrint: false,
                switchToDeviceTab: false,
                autoRefill: false,
                bedType: 'btPTE',
                currentProjectPrinterModel: '',
                filamentList: []
            },

            // MMS info (separated from printInfo for independent loading)
            mmsInfo: {
                mmsSystemName: '',
                mmsList: [
                    {
                        mmsId: '',
                        mmsName: '',
                        trayList: [
                            {
                                trayId: '',
                                trayName: '',
                                filamentType: '',
                                filamentName: '',
                                filamentColor: '',
                            }
                        ]
                    }
                ]
            },

            // Printer management
            printerList: [],
            curPrinter: null,
            // Additional printers receiving the same sliced file in this send.
            additionalPrinterIds: [],
            sending: false,
            slicedFilamentCount: 0,
            // Per-printer tray state and filament mapping, keyed by printerId. Each entry:
            // { printerName, mmsInfo, hasMms, filamentList, loading, error }
            additionalPrinterData: {},

            // Bed type management
            bedTypes: [
                { value: 'btPTE', name: this.$t ? this.$t('printSend.texturedA') : 'Textured A', icon: 'img/bed_pte_a.png' },
                { value: 'btPC', name: this.$t ? this.$t('printSend.smoothB') : 'Smooth B', icon: 'img/bed_pei_b.png' }
            ],
            selectedBedType: null,
            hasMmsInfo: false,
            // Filament management
            currentPage: 1,
            pageSize: 5,
            // UI state
            isEditingName: false,
            showFilamentSection: false,
            isIniting: false
        };
    },

    computed: {

        modelImageSrc() {
            return this.printInfo.thumbnail
                ? `data:image/png;base64,${this.printInfo.thumbnail}`
                : '';
        },

        showBedDropdown() {
            return this.printInfo.uploadAndPrint;
        },

        showPrintOptions() {
            return this.printInfo.uploadAndPrint;
        },

        totalPages() {
            return Math.ceil(((this.printInfo.filamentList && this.printInfo.filamentList.length) || 0) / this.pageSize);
        },

        currentPageFilaments() {
            if (!this.printInfo.filamentList) return [];
            const start = (this.currentPage - 1) * this.pageSize;
            const end = Math.min(start + this.pageSize, this.printInfo.filamentList.length);
            return this.printInfo.filamentList.slice(start, end).map((filament) => ({
                ...filament
            }));
        },

        mmsFilamentList() {
            return (this.mmsInfo && this.mmsInfo.mmsList) || [];
        },

        networkPrinters() {
            return this.printerList.filter(printer => printer.networkType === 1);
        },

        localPrinters() {
            return this.printerList.filter(printer => printer.networkType === 0);
        },
        // Every printer except the primary; offline and wrong-model ones are not selectable
        additionalPrinterOptions() {
            if (!this.printerList || this.printerList.length < 2) return [];
            const currentId = this.curPrinter ? this.curPrinter.printerId : null;
            return this.printerList.filter(printer => printer.printerId !== currentId);
        },

        // Own section: must render even when the primary printer has no filament system
        showAdditionalPrinterMapping() {
            return this.printInfo
                && this.printInfo.uploadAndPrint
                && this.additionalPrinterIds.length > 0;
        },

        // Check if selected printer model does not match the current project printer model
        printerModelNotMatch() {
            return this.curPrinter && this.printInfo.currentProjectPrinterModel && this.curPrinter.printerModel !== this.printInfo.currentProjectPrinterModel;
        },

        // Check if printer is busy (not idle and not print completed)
        printerBusy() {
            if (!this.curPrinter) return false;
            const status = this.curPrinter.printerStatus;
            if(this.curPrinter.connectStatus !== 1) return true;// If not connected, consider busy
            // Status 0 = idle, Status 16 = print completed
            // Any other status means the printer is busy
            return status !== 0 && status !== 16;
        },

        // Check if printer is in print completed state
        printerPrintCompleted() {
            if (!this.curPrinter) return false;
            // Status 16 = print completed
            return this.curPrinter.printerStatus === 16;
        },

        // Computed property to generate error list
        errorList() {
            const errors = [];
            
            // Priority 1: Printer model mismatch
            if (this.printerModelNotMatch) {
                errors.push(this.$t('printSend.printerModelNotMatch'));
            }
            
            // Priority 2: Printer busy warning (non-idle and non-completed)
            if (this.printerBusy) {
                errors.push(this.$t('printSend.printerBusyWarning'));
            }
            
            // Priority 3: Print completed warning (only if "Upload and Print" is checked)
            if (this.printerPrintCompleted && this.printInfo.uploadAndPrint) {
                errors.push(this.$t('printSend.printCompleteWarning'));
            }
            
            return errors;
        }
    },

    methods: {
        // Lifecycle methods
        async init() {
            this.isIniting = true;
            await this.requestPrinterList();
            this.isIniting = false;
        },

        // IPC Communication methods
        // quiet: the caller reports the failure itself, so it is not announced twice
        async ipcRequest(method, params = {}, timeout = 10000, quiet = false) {
            try {
                const response = await nativeIpc.request(method, params, timeout);
                return response;
            } catch (error) {
                console.error(`IPC request failed for ${method}:`, error);
                if (!quiet) {
                    this.showStatusTip(error.message || 'Request failed');
                }
                throw error;
            }
        },

        // Communication with backend
        async requestPrintTask() {
            const loading = ElLoading.service({
                lock: true,
            });
            try {
                const params = {
                    printerId: this.curPrinter.printerId
                };
                const response = await this.ipcRequest('request_print_task', params);
                if (response && response.modelName) {
                    response.modelName = this.filterModelName(response.modelName);
                }
                this.printInfo = { ...this.printInfo, ...response };
                // filamentList is emptied when the primary has no MMS; the slice's own
                // count is what decides whether a printer needs somewhere to switch
                this.slicedFilamentCount = (response.filamentList || []).length;
            } catch (error) {
                // Failure: set to null to indicate request failed (not a valid state)
                this.printInfo = null;
                console.error('Failed to request print task:', error);
            } finally {
                loading.close();
            }
        },

        async requestMmsInfo() {         
            const loading = ElLoading.service({
                lock: true,
            });
            try {  
                const params = {
                    printerId: this.curPrinter.printerId
                };
                const response = await this.ipcRequest('request_mms_info', params);
                if(!response) {
                    this.mmsInfo = null;
                    this.printInfo.filamentList = [];
                    return;
                }
                this.mmsInfo = response.mmsInfo || null;
                this.printInfo.filamentList = response.mappedFilamentList || [];        
            } catch (error) {
                console.error('Failed to request MMS info:', error);
                // Failure: set to null to indicate request failed (not a valid state)
                this.mmsInfo = null;
            } finally {
                loading.close();
            }
        },
        async requestPrinterList() {
            // const loading = ElLoading.service({
            //     lock: true,
            // });
            try {
                const response = await this.ipcRequest('request_printer_list', {});
                this.printerList = response || [];
                await this.updatePrinterSelection();
            } catch (error) {
                console.error('Failed to request printer list:', error);
            } finally {
                // loading.close();
            }
        },

        async resizeWindow() {
            let expand = false;
            if (this.printInfo && this.printInfo.uploadAndPrint &&
                ((this.mmsInfo && this.mmsInfo.mmsList && this.mmsInfo.mmsList.length > 0) ||
                 this.additionalPrinterIds.length > 0)
            ) {
                expand = true;
            }
            try {
                nativeIpc.sendEvent('expand_window', { expand });
            } catch (error) {
                console.error('Failed to request window resize:', error);
            }
        },

        async refreshPrinterList(event, value) {
            if (event) {
                event.stopPropagation();
            }
            await this.requestPrinterList();
        },

        async updatePrinterSelection() {
            if (this.printerList.length === 0) return;
            let selectedPrinter;
            if (this.curPrinter && this.curPrinter.printerId) {
                // find the printer by printerId
                let cur = this.printerList.find(p => p.printerId === this.curPrinter.printerId);
                if (cur) {
                    selectedPrinter = cur;
                }
            }
            // if not found, find the selected printer
            if (!selectedPrinter) {
                selectedPrinter = this.printerList.find(p => p.selected);
                if (!selectedPrinter) {
                    selectedPrinter = this.printerList[0];
                }
            }
            this.curPrinter = selectedPrinter;
            await this.onPrinterChanged();
        },

        // Model name editing
        startEditing() {
            this.isEditingName = true;
            this.$nextTick(() => {
                this.$refs.modelNameInput.focus();
            });
        },

        stopEditing() {
            this.isEditingName = false;
            this.printInfo.modelName = this.filterModelName(this.printInfo.modelName);
            this.adjustModelNameWidth();
        },

        onModelNameInput(value) {
            const filtered = this.filterModelName(value);
            if (filtered !== value) {
                this.printInfo.modelName = filtered;
            }
            this.adjustModelNameWidth();
        },

        filterModelName(str) {
            if (!str) return '';
            // Remove characters not allowed in Windows filenames: \ / : * ? " < > |
            let filtered = str.replace(/[\\/:*?"<>|]/g, '');
            // Windows reserved device names
            const reservedNames = /^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$/i;
            if (reservedNames.test(filtered)) {
                filtered = filtered + '_';
            }
            // Remove trailing spaces and dots (not allowed in Windows filenames)
            filtered = filtered.replace(/[\s.]+$/, '');
            return filtered;
        },

        adjustModelNameWidth() {
            const modelNameInput = this.$refs.modelNameInput;
            if (!modelNameInput) return;
            const input = modelNameInput.input;
            const span = document.createElement('span');
            span.style.visibility = 'hidden';
            span.style.position = 'fixed';
            span.style.font = window.getComputedStyle(input).font;
            span.innerText = modelNameInput.ref.value || modelNameInput.ref.placeholder;
            document.body.appendChild(span);
            input.style.width = (span.offsetWidth + 20) + 'px';
            document.body.removeChild(span);
        },

        // Filament management
        prevPage() {
            if (this.currentPage > 1) {
                this.currentPage--;
            }
        },

        nextPage() {
            if (this.currentPage < this.totalPages) {
                this.currentPage++;
            }
        },

        getFilamentStyle(filament) {
            const color = filament.filamentColor || '#888';
            const textColor = this.getContrastColor(color);
            return {
                background: color,
                color: textColor,
            };
        },

        getFilamentSvg(filament) {
            const color = filament.filamentColor || '#888';
            return getFilamentSvg(color);
        },

        getMmsCardStyle(mmsFilament) {
            const color = mmsFilament.filamentColor || '#888';
            const textColor = this.getContrastColor(color);
            return {
                background: color,
                color: textColor,
                border: `1px solid ${textColor}`
            };
        },

        getContrastColor(hexColor) {
            hexColor = hexColor.replace('#', '');
            if (hexColor.length === 3) {
                hexColor = hexColor.split('').map(x => x + x).join('');
            }
            const r = parseInt(hexColor.substr(0, 2), 16);
            const g = parseInt(hexColor.substr(2, 2), 16);
            const b = parseInt(hexColor.substr(4, 2), 16);
            const brightness = (r * 299 + g * 587 + b * 114) / 1000;
            return brightness > 180 ? '#222' : '#fff';
        },

        // MMS popover methods
        // Strip a reinforcement/grade suffix so PETG matches a PETG-CF tray.
        // Keep in sync with standardizeFilamentType() in PrintSendDialogEx.cpp.
        standardizeFilamentType(type) {
            let t = (type || '').toUpperCase().trim();
            if (t === '') return '';
            t = t.replace(/\+$/, '').trim();              // PLA+ -> PLA
            const dash = t.lastIndexOf('-');
            if (dash > 0) {
                const suffix = t.slice(dash + 1);
                // "-S" is deliberately not folded; see standardizeFilamentType() in
                // PrintSendDialogEx.cpp
                const isReinforcement = suffix === 'CF' || suffix === 'GF' || suffix === 'AERO';
                const isGrade = /^(CF|GF)\d+$/.test(suffix)      // PETG-CF10, UltraPA-CF25
                    || /^\d+[AD]$/.test(suffix);                 // TPU-95A, TPU-64D
                if (isReinforcement || isGrade) {
                    return t.slice(0, dash);
                }
            }
            return t;
        },

        // Check if the tray filament type matches the sliced filament type
        isFilamentTypeMatch(filament, tray) {
            if (!filament || !tray) return false;
            const trayType = this.standardizeFilamentType(tray.filamentType);
            if (trayType === '') return true;
            return this.standardizeFilamentType(filament.filamentType) === trayType;
        },

        // Plate chosen for this printer, falling back to the primary's
        getAdditionalBedType(printerId) {
            const entry = this.additionalPrinterData[printerId];
            const value = entry && entry.bedType;
            return this.bedTypes.find(b => b.value === value)
                || this.selectedBedType
                || this.bedTypes[0];
        },

        setAdditionalBedType(printerId, bedType) {
            const entry = this.additionalPrinterData[printerId];
            if (!entry || !bedType) return;
            entry.bedType = bedType.value;
        },

        // Same rule as resolveAdditionalPrinter: a mismatch only when both sides report a
        // model, so the dialog is never stricter than the backstop that refuses the send.
        isPrinterModelNotMatch(printer) {
            return !!(printer && this.printInfo && this.printInfo.currentProjectPrinterModel
                && printer.printerModel
                && printer.printerModel !== this.printInfo.currentProjectPrinterModel);
        },

        // Same rule as the printerBusy computed; a busy printer refuses the upload too,
        // so this is not conditional on Upload and Print
        isPrinterBusy(printer) {
            if (!printer) return false;
            // Not connected counts as busy
            if (printer.connectStatus !== 1) return true;
            return printer.printerStatus !== 0 && printer.printerStatus !== 16;
        },

        // A tray holds usable filament. Same rule as PrinterMmsManager::checkTrayIsReady:
        // loaded or preloaded, and the device reported what is in it.
        isTrayReady(tray) {
            return !!tray && (tray.status === 1 || tray.status === 3)
                && !!tray.filamentType && !!tray.filamentName && !!tray.filamentColor;
        },

        // Ready tray whose filament type does not match; selectable after confirmation
        isTrayTypeNotMatch(filament, tray) {
            return this.isTrayReady(tray) && !this.isFilamentTypeMatch(filament, tray);
        },

        // Printer and filament names come from the device; the confirm dialog renders HTML
        escapeHtml(text) {
            const div = document.createElement('div');
            div.textContent = text === undefined || text === null ? '' : String(text);
            return div.innerHTML;
        },

        // Skip printers that cannot start printing, with confirmation
        async confirmPartialSend(blocked, remaining) {
            try {
                // one message per count: vue-i18n pluralises a single count each
                await DialogHelper.confirm({
                    title: this.$t('printSend.partialSendTitle', blocked.length),
                    message: this.$t('printSend.partialSendSkipped', blocked.length) + '<br>' +
                        blocked.map(item => this.escapeHtml(item)).join('<br>') + '<br>' +
                        this.$t('printSend.partialSendRemaining', [remaining], remaining),
                    confirmText: this.$t('printSend.partialSendConfirm'),
                    cancelText: this.$t('printSend.cancel')
                });
                return true;
            } catch (e) {
                return false;
            }
        },

        // Confirm the beds of printers that have finished a print are clear
        async confirmBedsCleared(printers) {
            try {
                await DialogHelper.confirm({
                    title: this.$t('printSend.bedNotClearedTitle', printers.length),
                    message: this.$t('printSend.bedNotClearedDetail', printers.length) + '<br>' +
                        printers.map(p => this.escapeHtml(p.printerName)).join('<br>'),
                    confirmText: this.$t('printSend.bedNotClearedConfirm', printers.length),
                    cancelText: this.$t('printSend.cancel')
                });
                return true;
            } catch (e) {
                return false;
            }
        },

        // Confirm using a tray whose filament type does not match the slice
        async confirmFilamentTypeNotMatch(filament, tray) {
            try {
                await DialogHelper.confirm({
                    title: this.$t('printSend.filamentTypeNotMatch'),
                    message: this.$t('printSend.filamentTypeNotMatchDetail',
                        [this.escapeHtml(filament.filamentType || '?'),
                         this.escapeHtml(tray.trayName || '?'),
                         this.escapeHtml(tray.filamentType || '?')]),
                    confirmText: this.$t('printSend.filamentTypeNotMatchConfirm'),
                    cancelText: this.$t('printSend.cancel')
                });
                return true;
            } catch (e) {
                return false;
            }
        },

        // Request filament information for newly selected printers
        async syncAdditionalPrinterData() {
            const wanted = this.additionalPrinterIds.filter(id => !!id);
            Object.keys(this.additionalPrinterData).forEach(id => {
                if (!wanted.includes(id)) {
                    delete this.additionalPrinterData[id];
                }
            });

            for (const printerId of wanted) {
                if (this.additionalPrinterData[printerId]) continue;
                const printer = (this.printerList || []).find(p => p.printerId === printerId);
                this.additionalPrinterData[printerId] = {
                    printerName: printer ? printer.printerName : printerId,
                    mmsInfo: null,
                    hasMms: false,
                    filamentList: [],
                    loading: true,
                    error: '',
                    bedType: '',
                    readFailed: false
                };
                await this.fetchAdditionalPrinterData(printerId);
            }
        },

        // Takes an id, not the entry: the proxy must be read back here, or the identity
        // checks below compare against a raw object and never match
        async fetchAdditionalPrinterData(printerId) {
            const entry = this.additionalPrinterData[printerId];
            if (!entry) return;
            try {
                const response = await this.ipcRequest('request_additional_mms_info', { printerId }, 10000, true);
                // the entry can be deleted and recreated under the same id while this is in flight
                if (this.additionalPrinterData[printerId] !== entry) return;
                if (!response) {
                    entry.error = this.$t('printSend.filamentError');
                    entry.readFailed = true;
                } else {
                    entry.mmsInfo = response.mmsInfo || null;
                    entry.hasMms = response.hasMms;
                    entry.filamentList = response.mappedFilamentList || [];
                }
            } catch (error) {
                console.error('Failed to request filament information:', error);
                if (this.additionalPrinterData[printerId] === entry) {
                    entry.error = this.$t('printSend.filamentError');
                    entry.readFailed = true;
                }
            } finally {
                if (this.additionalPrinterData[printerId] === entry) {
                    entry.loading = false;
                }
            }
        },

        // Click anywhere on the card toggles it
        toggleAdditionalPrinter(printer) {
            if (!printer) return;
            const index = this.additionalPrinterIds.indexOf(printer.printerId);
            // an offline or wrong-model printer cannot be chosen, but one already chosen
            // that later reads as either must still be removable
            if (index === -1 && (printer.connectStatus !== 1 || this.isPrinterModelNotMatch(printer))) return;
            if (index === -1) {
                this.additionalPrinterIds.push(printer.printerId);
            } else {
                this.additionalPrinterIds.splice(index, 1);
            }
            this.syncAdditionalPrinterData();
            // push/splice leave the array identity alone, so a watcher on it never fires
            this.resizeWindow();
        },

        // Re-read one printer's trays, mutating in place so its chosen plate survives
        async refreshAdditionalPrinter(printerId) {
            if (this.sending) return;
            const entry = this.additionalPrinterData[printerId];
            if (!entry || entry.loading) return;
            entry.error = '';
            entry.readFailed = false;
            entry.loading = true;
            await this.fetchAdditionalPrinterData(printerId);
        },

        async updateAdditionalFilamentMapping(printerId, filamentIndex, tray) {
            const entry = this.additionalPrinterData[printerId];
            if (!entry) return;
            const filament = entry.filamentList.find(f => f.index === filamentIndex);
            if (!filament) return;
            if (!this.isTrayReady(tray)) return;

            let materialOverride = false;
            if (!this.isFilamentTypeMatch(filament, tray)) {
                if (!await this.confirmFilamentTypeNotMatch(filament, tray)) return;
                materialOverride = true;
            }
            const filamentSection = document.getElementsByClassName('filament-section');
            if (filamentSection && filamentSection[0]) {
                filamentSection[0].click();
            }
            filament.materialOverride = materialOverride;
            filament.mappedMmsFilament = Object.assign({}, filament.mappedMmsFilament, {
                filamentColor: tray.filamentColor,
                filamentName: tray.filamentName,
                filamentType: tray.filamentType,
                trayName: tray.trayName,
                mmsId: tray.mmsId,
                trayId: tray.trayId,
                filamentWeight: tray.filamentWeight,
                filamentDensity: tray.filamentDensity,
                minNozzleTemp: tray.minNozzleTemp,
                maxNozzleTemp: tray.maxNozzleTemp,
                minBedTemp: tray.minBedTemp,
                maxBedTemp: tray.maxBedTemp,
                status: tray.status,
                filamentDiameter: tray.filamentDiameter,
                filamentId: tray.filamentId,
                vendor: tray.vendor,
                serialNumber: tray.serialNumber,
                settingId: tray.settingId,
                from: tray.from
            });
        },

        // Check if every filament on this additional printer has a tray assigned
        checkAdditionalFilamentMapping(printerId) {
            const entry = this.additionalPrinterData[printerId];
            if (!entry || !entry.hasMms) return true;
            return entry.filamentList.every(filament =>
                filament.mappedMmsFilament &&
                (filament.mappedMmsFilament.trayName || '').trim() !== '' &&
                (filament.mappedMmsFilament.filamentColor || '').trim() !== '' &&
                (filament.mappedMmsFilament.filamentName || '').trim() !== '' &&
                (filament.mappedMmsFilament.filamentType || '').trim() !== '' &&
                (filament.mappedMmsFilament.mmsId || '').trim() !== '' &&
                (filament.mappedMmsFilament.trayId || '').trim() !== '');
        },

        async updateFilamentMapping(filamentIndex, tray) {
            const filamentSection = document.getElementsByClassName('filament-section');
            if (filamentSection && filamentSection[0]) {
                filamentSection[0].click();
            }
            console.log('Selected MMS Tray:', tray);
            for (let i = 0; i < this.printInfo.filamentList.length; i++) {
                if (this.printInfo.filamentList[i].index === filamentIndex) {
                    const filament = this.printInfo.filamentList[i];
                    if (!this.isTrayReady(tray)) return;

                    let materialOverride = false;
                    if (!this.isFilamentTypeMatch(filament, tray)) {
                        if (!await this.confirmFilamentTypeNotMatch(filament, tray)) return;
                        materialOverride = true;
                    }
                    filament.materialOverride = materialOverride;
                    filament.mappedMmsFilament.filamentColor = tray.filamentColor;
                    filament.mappedMmsFilament.filamentName = tray.filamentName;
                    filament.mappedMmsFilament.filamentType = tray.filamentType;
                    filament.mappedMmsFilament.trayName = tray.trayName;
                    filament.mappedMmsFilament.mmsId = tray.mmsId;
                    filament.mappedMmsFilament.trayId = tray.trayId;
                    filament.mappedMmsFilament.filamentWeight = tray.filamentWeight;
                    filament.mappedMmsFilament.filamentDensity = tray.filamentDensity;
                    filament.mappedMmsFilament.minNozzleTemp = tray.minNozzleTemp;
                    filament.mappedMmsFilament.maxNozzleTemp = tray.maxNozzleTemp;
                    filament.mappedMmsFilament.minBedTemp = tray.minBedTemp;
                    filament.mappedMmsFilament.maxBedTemp = tray.maxBedTemp;
                    filament.mappedMmsFilament.status = tray.status;
                    filament.mappedMmsFilament.filamentDiameter = tray.filamentDiameter;
                    filament.mappedMmsFilament.filamentId = tray.filamentId;
                    filament.mappedMmsFilament.vendor = tray.vendor;
                    filament.mappedMmsFilament.serialNumber = tray.serialNumber;
                    filament.mappedMmsFilament.settingId = tray.settingId;
                    filament.mappedMmsFilament.from = tray.from;
                    break;
                }
            }

        },

        // Action methods
        async cancel() {
            try {
                nativeIpc.sendEvent('cancel_print', {});
            } catch (error) {
                console.error('Failed to cancel print:', error);
            }
        },

        async upload() {
            if(this.curPrinter.connectStatus !== 1) {
                this.showStatusTip(this.$t('printSend.printerNotConnected'));
                return;
            }
            // Check if print task data is available
            if(this.printInfo === null) {
                this.showStatusTip(this.$t('printSend.printerNotConnected'));
                return;
            }
            //Validate filament mapping if MMS is present
            if (this.printInfo.uploadAndPrint && this.curPrinter.systemCapabilities.supportsMultiFilament && this.mmsInfo === null) {
                this.showStatusTip(this.$t('printSend.filamentError'));
                return;
            }             
            // Update task with current UI state
            this.printInfo.selectedPrinterId = this.curPrinter.printerId;
            this.printInfo.bedType = this.selectedBedType ? this.selectedBedType.value : 'btPTE';

            if (this.printInfo.uploadAndPrint && this.hasMmsInfo && !this.checkFilamentMapping()) {
                this.showStatusTip(this.$t('printSend.someFilamentsNotMapped'));
                return;
            }

            // Skip additional printers that cannot take the job, with confirmation
            let readyPrinterIds = this.additionalPrinterIds.filter(
                id => id !== this.printInfo.selectedPrinterId);
            const blocked = [];

            // Busy first, so a working printer is named for that rather than for the
            // transient tray state it reports mid-print
            readyPrinterIds = readyPrinterIds.filter(id => {
                const printer = (this.printerList || []).find(p => p.printerId === id);
                if (this.isPrinterBusy(printer)) {
                    blocked.push(this.$t('printSend.printerBusyFor', [printer.printerName,
                        this.getPrinterStatus(printer.printerStatus, printer.connectStatus)]));
                    return false;
                }
                return true;
            });

            if (this.printInfo.uploadAndPrint) {

                if (readyPrinterIds.some(id => {
                    const e = this.additionalPrinterData[id];
                    return !e || e.loading;
                })) {
                    this.showStatusTip(this.$t('printSend.stillLoadingPrinter'));
                    return;
                }

                readyPrinterIds = readyPrinterIds.filter(id => {
                    const entry = this.additionalPrinterData[id];

                    if (entry.readFailed) {
                        blocked.push(this.$t('printSend.printerFilamentUnreadable', [entry.printerName]));
                        return false;
                    }
                    if (!this.checkAdditionalFilamentMapping(id)) {
                        blocked.push(this.$t('printSend.printerFilamentsNotMapped', [entry.printerName]));
                        return false;
                    }
                    // A multi-filament print needs somewhere to switch, so a printer whose
                    // MMS is offline cannot run it - resolveAdditionalPrinter refuses it too
                    const mmsPrinter = (this.printerList || []).find(p => p.printerId === id);
                    if (mmsPrinter && mmsPrinter.systemCapabilities
                        && mmsPrinter.systemCapabilities.supportsMultiFilament
                        && !entry.hasMms && this.slicedFilamentCount > 1) {
                        blocked.push(this.$t('printSend.printerMmsNotConnectedFor', [entry.printerName]));
                        return false;
                    }
                    return true;
                });

            }

            // Not conditional on Upload and Print: the gcode is specific to a printer model,
            // so resolveAdditionalPrinter refuses a mismatch for a plain upload too.
            readyPrinterIds = readyPrinterIds.filter(id => {
                const printer = (this.printerList || []).find(p => p.printerId === id);
                if (this.isPrinterModelNotMatch(printer)) {
                    blocked.push(this.$t('printSend.printerModelNotMatchFor', [printer.printerName]));
                    return false;
                }
                return true;
            });

            if (blocked.length > 0) {
                const remaining = readyPrinterIds.length + 1; // + the primary
                const proceed = await this.confirmPartialSend(blocked, remaining);
                if (!proceed) return;
            }

            // A printer that has finished still has the last part on its bed. The primary
            // warns; an extra's card cannot - status 16 is styled the same green as idle.
            // Asked last, so it only covers printers this send is actually going to.
            if (this.printInfo.uploadAndPrint) {
                const occupied = readyPrinterIds
                    .map(id => (this.printerList || []).find(p => p.printerId === id))
                    .filter(p => p && p.printerStatus === 16);
                if (occupied.length > 0 && !await this.confirmBedsCleared(occupied)) {
                    return;
                }
            }

            if (this.sending) return;
            this.sending = true;

            const loading = ElLoading.service({
                lock: true,
            });

            try {
                const uploadData = {
                    ...this.printInfo,

                    additionalPrinters: readyPrinterIds
                        .map(id => {
                            const entry = this.additionalPrinterData[id] || {};
                            const plate = this.getAdditionalBedType(id);
                            return {
                                printerId: id,
                                bedType: plate ? plate.value : '',
                                filamentList: entry.filamentList || []
                            };
                        }),
                };
                // onPrint re-queries every additional printer (IPC_REQUEST_TIMEOUT_SECONDS
                // apiece on the non-master path), so the default timeout is not enough
                await this.ipcRequest('start_upload', uploadData,
                    10000 + 5000 * uploadData.additionalPrinters.length, true);
            } catch (error) {
                console.error('Failed to start upload:', error);
                if (error === undefined || error.code === undefined) {
                    // No code means the request timed out. The send may still be running, so
                    // the button stays disabled rather than risk enqueueing the plate twice.
                    this.showStatusTip(this.$t('printSend.sendTimedOut'));
                } else {
                    this.showStatusTip(error.message || this.$t('printSend.sendFailed'));
                    this.sending = false;
                    // PRINTER_MMS_TRAY_CHANGED: re-read rather than leave stale picks up
                    if (error.code === 10015) {
                        this.requestMmsInfo();
                        this.additionalPrinterIds.forEach(id => this.refreshAdditionalPrinter(id));
                    }
                }
            } finally {
                loading.close();
            }
        },

        checkFilamentMapping() {
            return this.printInfo.filamentList.every(filament =>
                filament.mappedMmsFilament &&
                filament.mappedMmsFilament.trayName.trim() !== "" &&
                filament.mappedMmsFilament.filamentName.trim() !== "" &&
                filament.mappedMmsFilament.filamentType.trim() !== "" &&
                filament.mappedMmsFilament.mmsId.trim() !== "" &&
                filament.mappedMmsFilament.trayId.trim() !== ""
            );
        },

        // Utility methods
        formatWeight(weight) {
            return weight ? `${weight.toFixed(2)}g` : '0.0g';
        },

        showStatusTip(message) {
            if (window.ElementPlus && window.ElementPlus.ElMessage) {
                window.ElementPlus.ElMessage.error({
                    message: message,
                    duration: 5000,
                    showClose: true
                });
            }
        },

        // Printer status display methods (using shared utilities)
        canShowProgressText(printerStatus, connectStatus) {
            return PrinterStatusUtils.canShowProgressText(printerStatus, connectStatus);
        },

        getPrinterStatus(printerStatus, connectStatus) {
            return PrinterStatusUtils.getPrinterStatus(printerStatus, connectStatus, this.$t);
        },

        getPrinterStatusStyle(printerStatus, connectStatus) {
            return PrinterStatusUtils.getPrinterStatusStyle(printerStatus, connectStatus);
        },

        shouldShowWarningIcon(printerStatus, connectStatus) {
            return PrinterStatusUtils.shouldShowWarningIcon(printerStatus, connectStatus);
        },

        getWarningTooltip(printerStatus) {
            return PrinterStatusUtils.getWarningTooltip(printerStatus, this.$t);
        },

        getPrinterProgress(printer) {
            return PrinterStatusUtils.getPrinterProgress(printer);
        },

        getPrinterRemainingTime(printer) {
            return PrinterStatusUtils.getPrinterRemainingTime(printer);
        },

        // Event handlers called by external code
        async onPrinterChanged() {
            // Drop the new primary and any printers that have gone away
            const availableIds = (this.printerList || []).map(printer => printer.printerId);
            const currentId    = this.curPrinter ? this.curPrinter.printerId : null;
            this.additionalPrinterIds = this.additionalPrinterIds.filter(
                id => id !== currentId && availableIds.includes(id));
            this.syncAdditionalPrinterData();
            // Handle printer change logic if needed
            await this.requestPrintTask();

            if(this.curPrinter.systemCapabilities.supportsMultiFilament) {
               await this.requestMmsInfo();
            } 
         console.log("Printer changed to:", this.curPrinter);
            // Set bed type if provided
            if (this.printInfo.bedType) {
                const bedType = this.bedTypes.find(b => b.value === this.printInfo.bedType);
                this.selectedBedType = bedType || this.bedTypes[0];
            }

            // Check for MMS info
            if (this.curPrinter.systemCapabilities.supportsMultiFilament &&
                this.mmsInfo &&
                this.mmsInfo.connected &&
                this.mmsInfo.mmsList &&
                this.mmsInfo.mmsList.length > 0) {
                this.hasMmsInfo = true;
            } else {
                this.hasMmsInfo = false;
            }
            this.refreshShowFilamentSection();
            this.$nextTick(() => {
                this.adjustModelNameWidth();
            });
        },

        onBedTypeChanged(bedType) {
            // Handle bed type change logic if needed
            console.log("Selected bed type:", bedType);
        },

        refreshShowFilamentSection() {
            setTimeout(() => {
                this.showFilamentSection = this.printInfo.uploadAndPrint && this.hasMmsInfo;
            }, this.printInfo.uploadAndPrint && this.hasMmsInfo ? 100 : 0);
        }
    },

    mounted() {
        this.selectedBedType = this.bedTypes[0];

        // Initialize the application
        this.init();
        disableRightClickMenu();
    },

    watch: {
        'mmsInfo.mmsList': {
            async handler(newValue, oldValue) {
                this.resizeWindow();
                this.refreshShowFilamentSection();
            },
            immediate: false
        },
        'printInfo.uploadAndPrint': {
            async handler(newValue, oldValue) {
                // React to upload and print toggle changes
                if (!newValue) {
                    this.currentPage = 1; // Reset pagination when hiding filament section
                }
                this.resizeWindow();
                this.refreshShowFilamentSection();
            },
            immediate: false
        }

    }
};

const app = createApp(PrintSendApp)

// Register ElementPlus components globally
app.use(ElementPlus, {
    components: {
        ElInput,
        ElButton,
        ElPopover
    }
});

// Create and mount the Vue app
app.use(i18n);
app.mount('#app');

