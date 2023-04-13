# Usage (without baseline results): python3 local_runner.py %input file% -- %command to execute solution%
# Usage (with baseline results): python3 local_runner.py %input file% %baseline results file% -- %command to execute solution%
# Examples: python3 local_runner.py sample/01.txt -- ./baseline
#           python3 local_runner.py sample/01.txt sample/01_baseline.txt -- python3 my_solution.py

from collections import defaultdict, namedtuple
from subprocess import Popen, PIPE
from sys import argv, exit


VMType = namedtuple('VMType', 'numas cpu mem')
Placement = namedtuple('Placement', 'net rack rack_global pm pm_global numa1 numa2 partition')
VM = namedtuple('VM', 'type_id pg_id placement')


class NUMA:
    def __init__(self, cpu, mem):
        self.cpu_total = cpu
        self.mem_total = mem
        self.cpu_used = 0
        self.mem_used = 0

    def allocate(self, vmtype):
        self.cpu_used += vmtype.cpu
        self.mem_used += vmtype.mem
        assert self.cpu_used <= self.cpu_total, 'NUMA CPU capacity exceeded!'
        assert self.mem_used <= self.mem_total, 'NUMA memory capacity exceeded!'

    def release(self, vmtype):
        self.cpu_used -= vmtype.cpu
        self.mem_used -= vmtype.mem


class PG:
    def __init__(self, rack_aa, host_aa, net_a, rack_a):
        self.rack_aa = rack_aa
        self.host_aa = host_aa
        self.net_a = net_a
        self.rack_a = rack_a

        assert rack_aa == 0 or rack_a == 0, "Placement group can't have both rack antiaffinity and affinity!"
        
        self.rack_partition = {}
        self.rack_vm_count = defaultdict(int)
        self.host_vm_count = defaultdict(int)
        self.net_vm_count = defaultdict(int)
        self.group_net_domain = set()
        self.group_rack_id = set()

    def has_soft_constraints(self):
        return self.host_aa > 0 or self.net_a == 1 or self.rack_a == 1

    def host_ok(self, idx):
        return self.host_aa == 0 or self.host_vm_count[idx] <= self.host_aa
        
    def new_placement(self, placement, req_id):
        ok = True
        if self.rack_aa > 0:
            rack_id = placement.rack_global
            partition = placement.partition
            if self.rack_vm_count[rack_id] > 0:
                cur_partition = self.rack_partition[rack_id]
                assert cur_partition == partition, "Rack antiaffinity violated (request #{})!".format(req_id)
                self.rack_vm_count[rack_id] += 1
            else:
                self.rack_partition[rack_id] = partition
                self.rack_vm_count[rack_id] = 1
        if self.host_aa > 0:
            self.host_vm_count[placement.pm_global] += 1
        if self.net_a > 0:
            net = placement.net
            self.net_vm_count[net] += 1
            self.group_net_domain.add(net)
            if len(self.group_net_domain) > 1:
                ok = False
                assert self.net_a != 2, "Hard network affinity violated (request #{})!".format(req_id)
        if self.rack_a > 0:
            rack = placement.rack_global
            self.rack_vm_count[rack] += 1
            self.group_rack_id.add(rack)
            if len(self.group_rack_id) > 1:
                ok = False
                assert self.rack_a != 2, "Hard rack affinity violated (request #{})!".format(req_id)
        return ok

    def delete_placement(self, placement):
        if self.rack_aa > 0 or self.rack_a > 0:
            self.rack_vm_count[placement.rack_global] -= 1
        if self.host_aa > 0:
            self.host_vm_count[placement.pm_global] -= 1
        if self.net_a > 0:
            self.net_vm_count[placement.net] -= 1
            if self.net_vm_count[placement.net] == 0:
                self.group_net_domain.remove(placement.net)
        if self.rack_a > 0 and self.rack_vm_count[placement.rack_global] == 0:
            self.group_rack_id.remove(placement.rack_global)


def end_interaction(participant, placed, soft_fulfilled, soft_total, finished, baseline_score):
    if not finished:
        participant.stdin.write('4\n')
        try:
            participant.stdin.flush()
        except:
            pass
    participant.wait()
    print('Placed {} VMs. {} / {} VMs placed according to soft constraints.'.format(placed, soft_fulfilled, soft_total))
    print('{}ll requests processed.'.format('A' if finished else 'Not a'))
    if baseline_score is None:
        print("Can't compute solution score: no baseline score provided.")
    else:
        placed_b, soft_b = baseline_score
        score = 0.8 * (placed / max(1, placed_b)) + 0.2 * (soft_fulfilled / max(1, soft_b))
        print('Solution score = {:.3f}'.format(score * 1000))
        print('NOTE: Here test weight is 1.')
    exit(0)


def interact(input_name, participant_run_cmd, baseline_score):
    with open(input_name, 'r') as f:
        participant = Popen(participant_run_cmd, stdin=PIPE, stdout=PIPE, text=True, encoding='utf-8')
        n_net, n_rack, n_pm, n_numa = list(map(int, f.readline().rstrip('\r\n').split(' ')))
        participant.stdin.write('{} {} {} {}\n'.format(n_net, n_rack, n_pm, n_numa))
        numa_cpu = []
        numa_mem = []
        for _ in range(n_numa):
            cpu, mem = list(map(int, f.readline().rstrip('\r\n').split(' ')))
            numa_cpu.append(cpu)
            numa_mem.append(mem)
            participant.stdin.write('{} {}\n'.format(cpu, mem))
        numa = []
        for _ in range(n_net):
            net_numa = []
            for _ in range(n_rack):
                rack_numa = []
                for _ in range(n_pm):
                    pm_numa = []
                    for i in range(n_numa):
                        pm_numa.append(NUMA(numa_cpu[i], numa_mem[i]))
                    rack_numa.append(pm_numa)
                net_numa.append(rack_numa)
            numa.append(net_numa)
        n_types = int(f.readline().rstrip('\r\n'))
        participant.stdin.write('{}\n'.format(n_types))
        types = []
        for _ in range(n_types):
            nc, cpu, mem = list(map(int, f.readline().rstrip('\r\n').split(' ')))
            participant.stdin.write('{} {} {}\n'.format(nc, cpu, mem))
            types.append(VMType(nc, cpu, mem))
        participant.stdin.flush()
        req_id = 0
        pgs = []
        vms = []
        placed = 0
        soft_fulfilled = 0
        soft_total = 0
        while True:
            req_id += 1
            req_type = int(f.readline().rstrip('\r\n'))
            participant.stdin.write('{}\n'.format(req_type))
            if req_type == 1:
                data = list(map(int, f.readline().rstrip('\r\n').split(' ')))
                participant.stdin.write(' '.join(list(map(str, data))) + '\n')
                data += list(map(int, f.readline().rstrip('\r\n').split(' ')))
                participant.stdin.write(' '.join(list(map(str, data[3:]))) + '\n')
                participant.stdin.flush()
                pgs.append(PG(data[1], data[2], data[3], data[4]))
            elif req_type == 2:
                l, fl, g, p = list(map(int, f.readline().rstrip('\r\n').split(' ')))
                participant.stdin.write('{} {} {} {}\n'.format(l, fl, g, p))
                ids = list(map(int, f.readline().rstrip('\r\n').split(' ')))
                assert len(ids) == l
                participant.stdin.write(' '.join(list(map(str, ids))) + '\n')
                participant.stdin.flush()
                soft_ok = True
                for i in range(l):
                    line = participant.stdout.readline().rstrip(' \r\n')
                    if line == '-1':
                        assert i == 0, 'Partial placements are forbidden!'
                        end_interaction(participant, placed, soft_fulfilled, soft_total, False, baseline_score)
                        return
                    tokens = list(map(int, line.split(' ')))
                    net = -1
                    rack = -1
                    pm = -1
                    numa1 = -1
                    numa2 = -1
                    numa_count = 0
                    if len(tokens) == 4:
                        numa_count = 1
                        net, rack, pm, numa1 = tokens
                    else:
                        assert len(tokens) == 5
                        numa_count = 2
                        net, rack, pm, numa1, numa2 = tokens
                        assert 0 < numa2 and numa2 <= n_numa
                        assert numa1 != numa2, '2-NUMA VMs must be placed on different NUMAs!'
                    assert numa_count == types[fl - 1].numas, 'Wrong number of NUMAs in the output!'
                    assert 0 < net and net <= n_net
                    assert 0 < rack and rack <= n_rack
                    assert 0 < pm and pm <= n_pm
                    assert 0 < numa1 and numa1 <= n_numa
                    net -= 1
                    rack -= 1
                    pm -= 1
                    numa1 -= 1
                    if numa2 != -1:
                        numa2 -= 1
                    partition = i if p == -1 else p - 1
                    place = Placement(net, rack, rack + net * n_rack, pm, pm + n_pm * (rack + net * n_rack), numa1, numa2, partition)
                    vm = VM(fl - 1, g - 1, place)
                    vms.append(vm)
                    numa[net][rack][pm][numa1].allocate(types[fl - 1])
                    if numa2 != -1:
                        numa[net][rack][pm][numa2].allocate(types[fl - 1])
                    status = pgs[g - 1].new_placement(place, req_id)
                    if pgs[g - 1].has_soft_constraints():
                        soft_total += 1
                        if not status:
                            soft_ok = False
                    else:
                        soft_ok = False
                placed += l
                if soft_ok:
                    if pgs[g - 1].host_aa > 0:
                        for i in range(len(vms) - l, len(vms)):
                            if pgs[g - 1].host_ok(vms[i].placement.pm_global):
                                soft_fulfilled += 1
                    else:
                        soft_fulfilled += l
            elif req_type == 3:
                ids = list(map(int, f.readline().rstrip('\r\n').split(' ')))
                participant.stdin.write(' '.join(list(map(str, ids))) + '\n')
                participant.stdin.flush()
                ids = ids[1:]
                for i in ids:
                    vm = vms[i - 1]
                    p = vm.placement
                    pgs[vm.pg_id].delete_placement(p)
                    numa[p.net][p.rack][p.pm][p.numa1].release(types[vm.type_id])
                    if p.numa2 != -1:
                        numa[p.net][p.rack][p.pm][p.numa2].release(types[vm.type_id])
            else:
                try:
                    participant.stdin.flush()
                except:
                    pass
                assert req_type == 4
                end_interaction(participant, placed, soft_fulfilled, soft_total, True, baseline_score)
                break


def read_baseline_score(filename):
    with open(filename, 'r') as f:
        placement, soft = list(map(int, f.readline().rstrip(' \r\n').split(' ')))
        return (placement, soft)


if __name__ == '__main__':
    split = argv.index('--')
    assert split == 2 or split == 3
    test = argv[1]
    solution = argv[split+1:]
    if split == 2:
        interact(test, solution, None)
    else:
        interact(test, solution, read_baseline_score(argv[2]))
