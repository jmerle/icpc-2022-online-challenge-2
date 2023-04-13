#include <chrono>
#include <deque>
#include <ios>
#include <iostream>
#include <numeric>
#include <optional>
#include <ostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef LOCAL
#define log if (true) std::cerr
#else
#define log if (false) std::cerr
#endif

struct PM;
struct Rack;
struct Domain;
struct VM;

template<typename T>
std::ostream &operator<<(std::ostream &os, const std::vector<T> &vec) {
    os << '[';

    for (int i = 0; i < vec.size(); i++) {
        os << vec[i];

        if (i != vec.size() - 1) {
            os << ", ";
        }
    }

    os << ']';
    return os;
}

enum class Affinity {
    NONE = 0,
    SOFT = 1,
    HARD = 2
};

struct Type {
    int nodes;
    int cpu;
    int memory;

    Type(int nodes, int cpu, int memory) : nodes(nodes), cpu(cpu), memory(memory) {}
};

struct WithResources {
    int totalCPU;
    int totalMemory;

    int availableCPU;
    int availableMemory;

    WithResources(int cpu, int memory)
            : totalCPU(cpu), totalMemory(memory), availableCPU(cpu), availableMemory(memory) {}

    void claimResources(Type *type) {
        availableCPU -= type->cpu;
        availableMemory -= type->memory;
    }

    void releaseResources(Type *type) {
        availableCPU += type->cpu;
        availableMemory += type->memory;
    }

    [[nodiscard]] bool hasResources(int cpu, int memory) const {
        return availableCPU >= cpu && availableMemory >= memory;
    }

    [[nodiscard]] bool hasResources(const Type &type) const {
        return hasResources(type.nodes * type.cpu, type.nodes * type.memory);
    }

    [[nodiscard]] double getLoad() const {
        double loadCPU = (double) (totalCPU - availableCPU) / totalCPU;
        double loadMemory = (double) (totalMemory - availableMemory) / totalMemory;

        return (loadCPU + loadMemory) / 2;
    }
};

struct Node : WithResources {
    int index;
    PM *pm;

    Node(int index, PM *pm, int cpu, int memory) : WithResources(cpu, memory), index(index), pm(pm) {}
};

struct PM : WithResources {
    int index;
    Rack *rack;

    std::vector<Node> nodes;
    std::unordered_map<int, int> vmsByPG;

    PM(int index, Rack *rack, const std::vector<int> &nodeCPU, const std::vector<int> &nodeMemory)
            : WithResources(std::accumulate(nodeCPU.begin(), nodeCPU.end(), 0),
                            std::accumulate(nodeMemory.begin(), nodeMemory.end(), 0)),
              index(index),
              rack(rack) {
        nodes.reserve(nodeCPU.size());
        for (int i = 0; i < nodeCPU.size(); i++) {
            nodes.emplace_back(i + 1, this, nodeCPU[i], nodeMemory[i]);
        }
    }
};

struct Rack : WithResources {
    int index;
    Domain *domain;

    std::vector<PM> pms;

    Rack(int index, Domain *domain, int noPMs, const std::vector<int> &nodeCPU, const std::vector<int> &nodeMemory)
            : WithResources(noPMs * std::accumulate(nodeCPU.begin(), nodeCPU.end(), 0),
                            noPMs * std::accumulate(nodeMemory.begin(), nodeMemory.end(), 0)),
              index(index),
              domain(domain) {
        pms.reserve(noPMs);
        for (int i = 0; i < noPMs; i++) {
            pms.emplace_back(i + 1, this, nodeCPU, nodeMemory);
        }
    }
};

struct Domain : WithResources {
    int index;

    std::vector<Rack> racks;

    Domain(int index, int noRacks, int noPMs, const std::vector<int> &nodeCPU, const std::vector<int> &nodeMemory)
            : WithResources(noRacks * noPMs * std::accumulate(nodeCPU.begin(), nodeCPU.end(), 0),
                            noRacks * noPMs * std::accumulate(nodeMemory.begin(), nodeMemory.end(), 0)),
              index(index) {
        racks.reserve(noRacks);
        for (int i = 0; i < noRacks; i++) {
            racks.emplace_back(i + 1, this, noPMs, nodeCPU, nodeMemory);
        }
    }
};

struct PG {
    int index;
    int hardRackAntiAffinityPartitions;
    int softPMAntiAffinity;
    Affinity domainAffinity;
    Affinity rackAffinity;

    std::vector<VM *> vms;

    Domain *targetDomain = nullptr;
    bool domainAffinityPossible = true;

    Rack *targetRack = nullptr;
    bool rackAffinityPossible = true;

    std::unordered_map<int, std::unordered_set<Rack *>> partitionRacks;

    PG(int index,
       int hardRackAntiAffinityPartitions,
       int softPMAntiAffinity,
       Affinity domainAffinity,
       Affinity rackAffinity)
            : index(index),
              hardRackAntiAffinityPartitions(hardRackAntiAffinityPartitions),
              softPMAntiAffinity(softPMAntiAffinity),
              domainAffinity(domainAffinity),
              rackAffinity(rackAffinity) {}

    void updateTargets();
};

struct VM {
    int index;
    Type *type;
    PG *pg;
    int partition;

    std::vector<Node *> nodes;

    VM(int index, Type *type, PG *pg, int partition) : index(index), type(type), pg(pg), partition(partition) {}

    void place(const std::vector<Node *> &placement) {
        nodes = placement;

        for (Node *node : nodes) {
            node->claimResources(type);
            node->pm->claimResources(type);
            node->pm->rack->claimResources(type);
            node->pm->rack->domain->claimResources(type);
        }

        nodes[0]->pm->vmsByPG[pg->index]++;
    }

    void unplace() {
        for (Node *node : nodes) {
            node->releaseResources(type);
            node->pm->releaseResources(type);
            node->pm->rack->releaseResources(type);
            node->pm->rack->domain->releaseResources(type);
        }

        nodes[0]->pm->vmsByPG[pg->index]--;

        nodes.clear();
    }

    [[nodiscard]] bool isPlaced() const {
        return !nodes.empty();
    }
};

void PG::updateTargets() {
    targetDomain = nullptr;
    domainAffinityPossible = true;

    targetRack = nullptr;
    rackAffinityPossible = true;

    partitionRacks.clear();

    for (VM *vm : vms) {
        if (!vm->isPlaced()) {
            continue;
        }

        Rack *rack = vm->nodes[0]->pm->rack;
        Domain *domain = rack->domain;

        if (domainAffinity != Affinity::NONE && domainAffinityPossible) {
            if (targetDomain == nullptr) {
                targetDomain = domain;
            } else if (targetDomain != domain) {
                domainAffinityPossible = false;
            }
        }

        if (rackAffinity != Affinity::NONE && rackAffinityPossible) {
            if (targetRack == nullptr) {
                targetRack = rack;
            } else if (targetRack != rack) {
                rackAffinityPossible = false;
            }
        }

        if (hardRackAntiAffinityPartitions > 0) {
            partitionRacks[vm->partition].insert(rack);
        }
    }
}

struct Placement {
    std::unordered_map<int, std::vector<Node *>> placements;
    double penalty;

    Placement() : placements(), penalty() {}

    Placement(const std::unordered_map<int, std::vector<Node *>> &placements, double penalty)
            : placements(placements), penalty(penalty) {}
};

struct Manager {
    int noDomains;
    int noRacks;
    int noPMs;
    int noNodes;
    int noTypes;

    std::vector<Type> types;

    std::vector<Domain> domains;

    std::unordered_map<int, PG> pgsByIndex;
    std::unordered_map<int, VM> vmsByIndex;

    std::chrono::high_resolution_clock::time_point startTime;

    Manager(int noDomains,
            int noRacks,
            int noPMs,
            const std::vector<int> &nodeCPU,
            const std::vector<int> &nodeMemory,
            const std::vector<Type> &types)
            : noDomains(noDomains),
              noRacks(noRacks),
              noPMs(noPMs),
              noNodes(nodeCPU.size()),
              noTypes(types.size()),
              types(types),
              startTime(std::chrono::high_resolution_clock::now()) {
        domains.reserve(noDomains);
        for (int i = 0; i < noDomains; i++) {
            domains.emplace_back(i + 1, noRacks, noPMs, nodeCPU, nodeMemory);
        }
    }

    void createPG(int index,
                  int hardRackAntiAffinityPartitions,
                  int softPMAntiAffinity,
                  Affinity domainAffinity,
                  Affinity rackAffinity) {
        pgsByIndex.emplace(index,
                           PG(index, hardRackAntiAffinityPartitions, softPMAntiAffinity, domainAffinity, rackAffinity));
    }

    bool createVMs(const std::vector<int> &indices, int typeIndex, int pgIndex, int partition) {
        auto currentTime = std::chrono::high_resolution_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(currentTime - startTime).count() >= 14) {
            log << "Timed out, terminating" << std::endl;
            std::cout << -1 << std::endl;
            return false;
        }

        Type &type = types.at(typeIndex - 1);
        PG &pg = pgsByIndex.at(pgIndex);

        std::vector<VM *> vmsToPlace;

        for (int i = 0; i < indices.size(); i++) {
            vmsByIndex.emplace(indices[i], VM(indices[i], &type, &pg, partition >= 0 ? partition : i + 1));
            VM &vm = vmsByIndex.at(indices[i]);

            pg.vms.push_back(&vm);
            vmsToPlace.push_back(&vm);
        }

        std::optional<Placement> bestPlacement;

        for (auto &racks : getRackGroups(pg)) {
            auto placement = getBestPlacement(pg, vmsToPlace, type, racks);
            if (!placement.has_value()) {
                continue;
            }

            log << "Penalty: " << placement->penalty << std::endl;

            if (!bestPlacement.has_value() || placement->penalty < bestPlacement->penalty) {
                bestPlacement = placement;
            }
        }

        unplaceVMs(vmsToPlace);

        if (!bestPlacement.has_value()) {
            log << "Cannot create VM(s), terminating" << std::endl;
            std::cout << -1 << std::endl;
            return false;
        }

        for (VM *vm : vmsToPlace) {
            std::vector<Node *> &nodes = bestPlacement->placements[vm->index];
            PM *pm = nodes[0]->pm;

            vm->place(nodes);

            std::cout << pm->rack->domain->index << ' ' << pm->rack->index << ' ' << pm->index;
            for (Node *node : nodes) {
                std::cout << ' ' << node->index;
            }
            std::cout << std::endl;
        }

        pg.updateTargets();
        return true;
    }

    void deleteVMs(const std::vector<int> &indices) {
        for (int index : indices) {
            VM &vm = vmsByIndex.at(index);
            vm.unplace();
            vm.pg->vms.erase(std::remove(vm.pg->vms.begin(), vm.pg->vms.end(), &vm), vm.pg->vms.end());
            vmsByIndex.erase(index);
        }
    }

    std::vector<std::vector<Rack *>> getRackGroups(PG &pg) {
        pg.updateTargets();

        std::vector<std::vector<Rack *>> groups;

        if (pg.rackAffinity == Affinity::HARD) {
            if (pg.targetRack != nullptr) {
                groups.push_back({pg.targetRack});
            } else {
                for (Domain &domain : domains) {
                    for (Rack &rack : domain.racks) {
                        groups.push_back({&rack});
                    }
                }
            }

            return groups;
        }

        if (pg.domainAffinity == Affinity::HARD) {
            if (pg.targetDomain != nullptr) {
                groups.emplace_back(getVectorPointers(pg.targetDomain->racks));
            } else {
                for (Domain &domain : domains) {
                    groups.emplace_back(getVectorPointers(domain.racks));
                }
            }

            return groups;
        }

        if (pg.rackAffinity == Affinity::SOFT && pg.rackAffinityPossible) {
            if (pg.targetRack != nullptr) {
                groups.push_back({pg.targetRack});
            } else {
                for (Domain &domain : domains) {
                    for (Rack &rack : domain.racks) {
                        groups.push_back({&rack});
                    }
                }
            }
        }

        if (pg.domainAffinity == Affinity::SOFT && pg.domainAffinityPossible) {
            if (pg.targetDomain != nullptr) {
                groups.emplace_back(getVectorPointers(pg.targetDomain->racks));
            } else {
                for (Domain &domain : domains) {
                    groups.emplace_back(getVectorPointers(domain.racks));
                }
            }
        }

        std::vector<Rack *> allRacks;
        for (Domain &domain : domains) {
            for (Rack &rack : domain.racks) {
                allRacks.push_back(&rack);
            }
        }

        groups.push_back(allRacks);

        return groups;
    }

    std::optional<Placement> getBestPlacement(PG &pg,
                                              const std::vector<VM *> &vmsToPlace,
                                              const Type &type,
                                              const std::vector<Rack *> &racks) const {
        Placement completePlacement;

        std::unordered_map<int, std::vector<VM *>> vmsByPartition;
        for (VM *vm : vmsToPlace) {
            vmsByPartition[vm->partition].push_back(vm);
        }

        for (auto &[partition, vms] : vmsByPartition) {
            unplaceVMs(vms);
            pg.updateTargets();

            std::deque<Rack *> sortedRacks(racks.begin(), racks.end());
            std::sort(sortedRacks.begin(), sortedRacks.end(), [](const Rack *a, const Rack *b) {
                return a->getLoad() < b->getLoad();
            });

            std::deque<Rack *> startRacks;
            std::deque<Rack *> extraRacks;

            if (partition > 0) {
                std::unordered_set<Rack *> invalidRacks;
                for (auto &[p, otherRacks] : pg.partitionRacks) {
                    if (p == partition) {
                        continue;
                    }

                    for (Rack *rack : otherRacks) {
                        invalidRacks.insert(rack);
                    }
                }

                for (Rack *rack : pg.partitionRacks[partition]) {
                    if (!invalidRacks.contains(rack)) {
                        startRacks.push_back(rack);
                    }
                }

                for (Rack *rack : racks) {
                    if (!invalidRacks.contains(rack)
                        && std::find(startRacks.begin(), startRacks.end(), rack) == startRacks.end()) {
                        extraRacks.push_back(rack);
                    }
                }

                if (startRacks.empty()) {
                    if (extraRacks.empty()) {
                        return {};
                    }

                    startRacks.push_back(extraRacks[0]);
                    extraRacks.pop_front();
                }
            } else if (pg.rackAffinity == Affinity::SOFT && pg.rackAffinityPossible) {
                if (pg.targetRack != nullptr && std::find(racks.begin(), racks.end(), pg.targetRack) != racks.end()) {
                    startRacks.push_back({pg.targetRack});
                    extraRacks = sortedRacks;
                    extraRacks.erase(std::remove(extraRacks.begin(), extraRacks.end(), pg.targetRack),
                                     extraRacks.end());
                } else {
                    startRacks.push_back({sortedRacks[0]});
                    extraRacks = sortedRacks;
                    extraRacks.pop_front();
                }
            } else {
                startRacks = sortedRacks;
            }

            for (bool force : {false, true}) {
                std::deque<Rack *> currentStartRacks = startRacks;
                std::deque<Rack *> currentExtraRacks = extraRacks;

                bool done = false;
                while (true) {
                    auto placement = tryPlace(pg, vms, type, currentStartRacks, force);

                    if (placement.has_value()) {
                        completePlacement.placements.insert(placement->placements.begin(), placement->placements.end());
                        completePlacement.penalty += placement->penalty;
                        done = true;
                        break;
                    }

                    if (currentExtraRacks.empty()) {
                        if (force) {
                            return {};
                        } else {
                            break;
                        }
                    }

                    currentStartRacks.push_back(currentExtraRacks[0]);
                    currentExtraRacks.pop_front();
                }

                if (done) {
                    break;
                }
            }
        }

        double totalLoad = 0.0;
        for (Rack *rack : racks) {
            totalLoad += rack->getLoad();
        }

        completePlacement.penalty += totalLoad / racks.size();

        return completePlacement;
    }

    std::optional<Placement> tryPlace(PG &pg,
                                      const std::vector<VM *> &vmsToPlace,
                                      const Type &type,
                                      std::deque<Rack *> &racks,
                                      bool force) const {
        unplaceVMs(vmsToPlace);

        int availableCPU = 0;
        int availableMemory = 0;
        for (Rack *rack : racks) {
            availableCPU += rack->availableCPU;
            availableMemory += rack->availableMemory;
        }

        if (vmsToPlace.size() * type.nodes * type.cpu > availableCPU
            || vmsToPlace.size() * type.nodes * type.memory > availableMemory) {
            return {};
        }

        std::unordered_map<int, std::vector<Node *>> placements;

        tryPlaceInner(pg, vmsToPlace, type, racks, placements, false);
        if (force && placements.size() < vmsToPlace.size()) {
            tryPlaceInner(pg, vmsToPlace, type, racks, placements, true);
        }

        if (placements.size() < vmsToPlace.size()) {
            return {};
        }

        int penalty = 0;
        pg.updateTargets();

        if (pg.softPMAntiAffinity > 0) {
            for (VM *vm : vmsToPlace) {
                if (vm->nodes[0]->pm->vmsByPG[pg.index] > pg.softPMAntiAffinity) {
                    penalty++;
                }
            }
        }

        if (pg.domainAffinity == Affinity::SOFT && !pg.domainAffinityPossible) {
            penalty += 1000;
        }

        if (pg.rackAffinity == Affinity::SOFT && !pg.rackAffinityPossible) {
            penalty += 1000;
        }

        return Placement(placements, penalty);
    }

    void tryPlaceInner(const PG &pg,
                       const std::vector<VM *> &vmsToPlace,
                       const Type &type,
                       std::deque<Rack *> &racks,
                       std::unordered_map<int, std::vector<Node *>> &placements,
                       bool force) const {
        for (VM *vm : vmsToPlace) {
            if (vm->isPlaced()) {
                continue;
            }

            std::sort(racks.begin(), racks.end(), [](const Rack *a, const Rack *b) {
                return a->getLoad() < b->getLoad();
            });

            for (Rack *rack : racks) {
                if (!rack->hasResources(type)) {
                    continue;
                }

                for (PM &pm : rack->pms) {
                    if (!pm.hasResources(type)) {
                        continue;
                    }

                    if (!force && pg.softPMAntiAffinity > 0 && pm.vmsByPG[pg.index] >= pg.softPMAntiAffinity) {
                        continue;
                    }

                    std::vector<Node *> nodes;
                    for (Node &node : pm.nodes) {
                        if (!node.hasResources(type.cpu, type.memory)) {
                            continue;
                        }

                        nodes.push_back(&node);

                        if (nodes.size() == type.nodes) {
                            break;
                        }
                    }

                    if (nodes.size() != type.nodes) {
                        continue;
                    }

                    placements[vm->index] = nodes;
                    vm->place(nodes);
                    break;
                }

                if (vm->isPlaced()) {
                    break;
                }
            }
        }
    }

    template<typename T>
    std::vector<T *> getVectorPointers(std::vector<T> &values) const {
        std::vector<T *> pointers(values.size());

        for (int i = 0; i < values.size(); i++) {
            pointers[i] = &values[i];
        }

        return pointers;
    }

    void unplaceVMs(const std::vector<VM *> &vms) const {
        for (VM *vm : vms) {
            if (vm->isPlaced()) {
                vm->unplace();
            }
        }
    }
};

int main() {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    int noDomains, noRacks, noPMs, noNodes;
    std::cin >> noDomains >> noRacks >> noPMs >> noNodes;

    log << "noDomains = " << noDomains
        << ", noRacks = " << noRacks
        << ", noPMs = " << noPMs
        << ", noNodes = " << noNodes
        << std::endl;

    std::vector<int> nodeCPU(noNodes);
    std::vector<int> nodeMemory(noNodes);
    for (int i = 0; i < noNodes; i++) {
        std::cin >> nodeCPU[i] >> nodeMemory[i];

        log << "Node " << (i + 1) << ": cpu = " << nodeCPU[i] << ", memory = " << nodeMemory[i] << std::endl;
    }

    int noTypes;
    std::cin >> noTypes;

    std::vector<Type> types;
    for (int i = 0; i < noTypes; i++) {
        int nodes, cpu, memory;
        std::cin >> nodes >> cpu >> memory;

        types.emplace_back(nodes, cpu, memory);

        log << "Type " << (i + 1) << ": nodes = " << nodes << ", cpu = " << cpu << ", memory = " << memory << std::endl;
    }

    Manager manager(noDomains, noRacks, noPMs, nodeCPU, nodeMemory, types);

    for (int requestId = 1;; requestId++) {
        log << "\nRequest " << requestId << ": ";

        int requestType;
        std::cin >> requestType;

        bool terminate = false;
        switch (requestType) {
            case 1: {
                int index, hardRackAntiAffinityPartitions, softPMAntiAffinity;
                std::cin >> index >> hardRackAntiAffinityPartitions >> softPMAntiAffinity;

                int domainAffinity, rackAffinity;
                std::cin >> domainAffinity >> rackAffinity;

                log << "Create PG\n"
                    << "index = " << index
                    << ", hardRackAntiAffinityPartitions = " << hardRackAntiAffinityPartitions
                    << ", softPMAntiAffinity = " << softPMAntiAffinity
                    << ", domainAffinity = " << domainAffinity
                    << ", rackAffinity = " << rackAffinity
                    << "\n----------"
                    << std::endl;

                manager.createPG(index,
                                 hardRackAntiAffinityPartitions,
                                 softPMAntiAffinity,
                                 static_cast<Affinity>(domainAffinity),
                                 static_cast<Affinity>(rackAffinity));
                break;
            }
            case 2: {
                int noVMs, typeIndex, pgIndex, partition;
                std::cin >> noVMs >> typeIndex >> pgIndex >> partition;

                std::vector<int> indices(noVMs);
                for (int i = 0; i < noVMs; i++) {
                    std::cin >> indices[i];
                }

                log << "Create VM(s)\n"
                    << "noVMs = " << noVMs
                    << ", typeIndex = " << typeIndex
                    << ", partition = " << partition
                    << "\nindices = " << indices
                    << "\n----------"
                    << std::endl;

                terminate = !manager.createVMs(indices, typeIndex, pgIndex, partition);
                break;
            }
            case 3: {
                int noVMs;
                std::cin >> noVMs;

                std::vector<int> indices(noVMs);
                for (int i = 0; i < noVMs; i++) {
                    std::cin >> indices[i];
                }

                log << "Delete VM(s)\n"
                    << "noVMs = " << noVMs
                    << "\nindices = " << indices
                    << "\n----------"
                    << std::endl;

                manager.deleteVMs(indices);
                break;
            }
            case 4: {
                log << "Terminate\n----------" << std::endl;
                terminate = true;
                break;
            }
        }

        if (terminate) {
            break;
        }
    }

    return 0;
}
