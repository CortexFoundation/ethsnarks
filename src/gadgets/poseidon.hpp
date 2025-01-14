#ifndef ETHSNARKS_POSEIDON_HPP_
#define ETHSNARKS_POSEIDON_HPP_

// Copyright (c) 2019 HarryR
// License: LGPL-3.0+

#include "ethsnarks.hpp"
#include "crypto/blake2b.h"

#include <mutex>

namespace ethsnarks {

using libsnark::linear_combination;
using libsnark::linear_term;


struct PoseidonConstants
{
	std::vector<FieldT> C; // `t` constants
	std::vector<FieldT> M; // `t * t` matrix of constants
};


class FifthPower_gadget : public GadgetT {
public:
	const VariableT x2;
	const VariableT x4;
	const VariableT x5;

	FifthPower_gadget(
		ProtoboardT &pb,
		const std::string& annotation_prefix
	) :
		GadgetT(pb, annotation_prefix),
		x2(make_variable(pb, FMT(annotation_prefix, ".x2"))),
		x4(make_variable(pb, FMT(annotation_prefix, ".x4"))),
		x5(make_variable(pb, FMT(annotation_prefix, ".x5")))
	{
	}

	void generate_r1cs_constraints(const linear_combination<FieldT>& x) const
	{
		pb.add_r1cs_constraint(ConstraintT(x, x, x2), ".x^2 = x * x");
		pb.add_r1cs_constraint(ConstraintT(x2, x2, x4), ".x^4 = x2 * x2");
		pb.add_r1cs_constraint(ConstraintT(x, x4, x5), ".x^5 = x * x4");
	}

	void generate_r1cs_witness(const FieldT& val_x) const
    {
    	const auto val_x2 = val_x * val_x;
    	const auto val_x4 = val_x2 * val_x2;
    	const auto val_x5 = val_x4 * val_x;
    	this->pb.val(x2) = val_x2;
    	this->pb.val(x4) = val_x4;
    	this->pb.val(x5) = val_x5;
    }

    const VariableT& result() const
    {
    	return x5;
    }
};


static void poseidon_constants_fill(const std::string &seed, unsigned n_constants, std::vector<FieldT> &result )
{
	blake2b_ctx ctx;

	const unsigned n_bits_roundedup = FieldT::size_in_bits() + (8 - (FieldT::size_in_bits()%8));
	const unsigned output_size = n_bits_roundedup / 8;
	uint8_t output[output_size];

	result.reserve(n_constants);

	blake2b(output, output_size, NULL, 0, seed.c_str(), seed.size());
	result.emplace_back( bytes_to_FieldT_littleendian(output, output_size) );

	for( unsigned i = 0; i < (n_constants - 1); i++ )
	{
		blake2b(output, output_size, NULL, 0, output, output_size);
		result.emplace_back( bytes_to_FieldT_littleendian(output, output_size) );
	}
}


static const std::vector<FieldT> poseidon_constants(const std::string &seed, unsigned n_constants)
{
	std::vector<FieldT> result;
	poseidon_constants_fill(seed, n_constants, result);
	return result;
}


static void poseidon_matrix_fill(const std::string &seed, unsigned t, std::vector<FieldT> &result)
{
	const std::vector<FieldT> c = poseidon_constants(seed, t*2);

	result.reserve(t*2);

	for( unsigned i = 0; i < t; i++ )
	{
		for( unsigned j = 0; j < t; j++ )
		{
			result.emplace_back((c[i] - c[t+j]).inverse());
		}
	}
}


static const std::vector<FieldT> poseidon_matrix(const std::string &seed, unsigned t)
{
	std::vector<FieldT> result;
	poseidon_matrix_fill(seed, t, result);
	return result;
}


template<unsigned param_t, unsigned param_F, unsigned param_P>
const PoseidonConstants& poseidon_params()
{
    static PoseidonConstants constants;
    static std::once_flag flag;

    std::call_once(flag, [](){
    	poseidon_constants_fill("poseidon_constants", param_F + param_P, constants.C);
        poseidon_matrix_fill("poseidon_matrix_0000", param_t, constants.M);
    });

    return constants;
}


/**
* One round of the Poseidon permutation:
*
*    - takes a state of `t` elements
*    - adds the round constant to each element in the state
*    - performs exponentiation on the first `n` elements of the state
*    - creates `o` outputs, mixed using a matrix vector transform
*
* This generic version can be used as either a 'full', 'partial' or 'last' round.
* It avoids computing as many constraints as is possible, given all the information.
*/
template<unsigned param_t, unsigned nSBox, unsigned nInputs, unsigned nOutputs>
class Poseidon_Round : public GadgetT {
public:
	const FieldT& C_i;
	const std::vector<FieldT>& M;
	const std::vector<libsnark::linear_combination<FieldT> > state;
	const std::vector<FifthPower_gadget> sboxes;
	const std::vector<libsnark::linear_combination<FieldT> > outputs;

	static std::vector<FifthPower_gadget> make_sboxes(
		ProtoboardT& in_pb,
		const std::string& annotation_prefix )
	{
		std::vector<FifthPower_gadget> ret;

		ret.reserve(nSBox);
		for( unsigned h = 0; h < nSBox; h++ )
		{
			ret.emplace_back( in_pb, FMT(annotation_prefix, ".sbox[%u]", h) );
		}

		return ret;
	}

	static std::vector<libsnark::linear_combination<FieldT> > make_outputs(
		const FieldT& in_C_i,
		const std::vector<FieldT>& in_M,
		const std::vector<libsnark::linear_combination<FieldT> >& in_state,
		const std::vector<FifthPower_gadget>& in_sboxes )
	{
		std::vector<libsnark::linear_combination<FieldT> > ret(nOutputs);

#ifdef MULTICORE
        #pragma omp parallel for
#endif
		for( unsigned i = 0; i < nOutputs; i++ )
		{
			const unsigned M_offset = i * param_t;

			// Any element which isn't passed through an sbox
			// Can be accumulated separately as part of the constant term
			FieldT constant_term;
			for( unsigned j = nSBox; j < param_t; j++ ) {
				constant_term += in_C_i * in_M[M_offset+j];
			}

			linear_combination<FieldT>& lc = ret[i];
			lc.terms.reserve(param_t);
			if( nSBox < param_t )
			{
				lc.add_term(libsnark::ONE, constant_term);
			}

			// Add S-Boxes to the output row
			for( unsigned s = 0; s < nSBox; s++ )
			{
				lc.add_term(in_sboxes[s].result(), in_M[M_offset+s]);
			}

			// Then add inputs (from the state) multiplied by the matrix element
			for( unsigned k = nSBox; k < nInputs; k++ )
			{
				lc = lc + (in_state[k] * in_M[M_offset+k]);
			}
		}
		return ret;
	}

	Poseidon_Round(
		ProtoboardT &in_pb,
		const FieldT& in_C_i,
		const std::vector<FieldT>& in_M,
		const VariableArrayT& in_state,
		const std::string& annotation_prefix
	) :
		Poseidon_Round(in_pb, in_C_i, in_M, VariableArrayT_to_lc(in_state), annotation_prefix)
	{ }

	Poseidon_Round(
		ProtoboardT &in_pb,
		const FieldT& in_C_i,
		const std::vector<FieldT>& in_M,
		const std::vector<libsnark::linear_combination<FieldT> >& in_state,
		const std::string& annotation_prefix
	) :
		GadgetT(in_pb, annotation_prefix),
		C_i(in_C_i),
		M(in_M),
		state(in_state),
		sboxes(make_sboxes(in_pb, annotation_prefix)),
		outputs(make_outputs(in_C_i, in_M, in_state, sboxes))
	{
		assert( nInputs <= param_t );
		assert( nOutputs <= param_t );
	}

	void generate_r1cs_witness() const
	{
		for( unsigned h = 0; h < nSBox; h++ )
		{
			auto value = C_i;
			if( h < nInputs ) {
				value += lc_val(this->pb, state[h]); // this->pb.val(state[h]);
			}
			sboxes[h].generate_r1cs_witness( value );
		}
	}

	void generate_r1cs_constraints() const
	{
		for( unsigned h = 0; h < nSBox; h++ )
		{
			if( h < nInputs ) {
				sboxes[h].generate_r1cs_constraints( state[h] + C_i );
			}
			else {
				sboxes[h].generate_r1cs_constraints( C_i );
			}
		}
	}
};


template<unsigned param_t, unsigned param_c, unsigned param_F, unsigned param_P, unsigned nInputs, unsigned nOutputs, bool constrainOutputs=true>
class Master_Poseidon_gadget_T : public GadgetT
{
protected:
	typedef Poseidon_Round<param_t, param_t, nInputs, param_t> FirstRoundT;    // ingests `nInput` elements, expands to `t` elements using round constants
	typedef Poseidon_Round<param_t, param_c, param_t, param_t> PartialRoundT;  // partial round only runs sbox on `c` elements (capacity)
	typedef Poseidon_Round<param_t, param_t, param_t, param_t> FullRoundT;     // full bandwidth
	typedef Poseidon_Round<param_t, param_t, param_t, nOutputs> LastRoundT;   // squeezes state into `nOutputs`

	typedef const std::vector<libsnark::linear_combination<FieldT> >& lc_outputs_t;
	typedef const libsnark::linear_combination<FieldT>& lc_output_t;
	typedef const VariableT& var_output_t;
	typedef const VariableArrayT& var_outputs_t;

	static constexpr unsigned partial_begin = (param_F/2);
	static constexpr unsigned partial_end = (partial_begin + param_P);
	static constexpr unsigned total_rounds = param_F + param_P;

public:

	const PoseidonConstants& constants;
	FirstRoundT first_round;
	std::vector<FullRoundT> prefix_full_rounds;
	std::vector<PartialRoundT> partial_rounds;
	std::vector<FullRoundT> suffix_full_rounds;
	LastRoundT last_round;

	// When `constrainOutputs==true`, need variables to store outputs
	const VariableArrayT _output_vars;

	template<typename T>
	static const std::vector<T> make_rounds(
		unsigned n_begin, unsigned n_end,
		ProtoboardT& pb,
		const std::vector<libsnark::linear_combination<FieldT> >& inputs,
		const PoseidonConstants& constants,
		const std::string& annotation_prefix)
	{
		std::vector<T> result;
		result.reserve(n_end - n_begin);

		for( unsigned i = n_begin; i < n_end; i++ )
		{
			const auto& state = (i == n_begin) ? inputs : result.back().outputs;
			result.emplace_back(pb, constants.C[i], constants.M, state, FMT(annotation_prefix, ".round[%u]", i));
		}

		return result;
	}

	Master_Poseidon_gadget_T(
		ProtoboardT& pb,
		const VariableArrayT& in_inputs,
		const std::string& annotation_prefix
	) :
		GadgetT(pb, annotation_prefix),
		constants(poseidon_params<param_t, param_F, param_P>()),
		first_round(pb, constants.C[0], constants.M, in_inputs, FMT(annotation_prefix, ".round[0]")),
		prefix_full_rounds(
			make_rounds<FullRoundT>(
				1, partial_begin, pb,
				first_round.outputs, constants, annotation_prefix)),
		partial_rounds(
			make_rounds<PartialRoundT>(
				partial_begin, partial_end, pb,
				prefix_full_rounds.back().outputs, constants, annotation_prefix)),
		suffix_full_rounds(
			make_rounds<FullRoundT>(
				partial_end, total_rounds-1, pb,
				partial_rounds.back().outputs, constants, annotation_prefix)),
		last_round(pb, constants.C.back(), constants.M, suffix_full_rounds.back().outputs, FMT(annotation_prefix, ".round[%u]", total_rounds-1)),
		_output_vars(constrainOutputs ? make_var_array(pb, nOutputs, ".output") : VariableArrayT())
	{

	}

	void generate_r1cs_constraints() const
	{
		first_round.generate_r1cs_constraints();

		for( auto& prefix_round : prefix_full_rounds ) {
			prefix_round.generate_r1cs_constraints();
		}

		for( auto& partial_round : partial_rounds ) {
			partial_round.generate_r1cs_constraints();
		}

		for( auto& suffix_round : suffix_full_rounds ) {
			suffix_round.generate_r1cs_constraints();
		}

		last_round.generate_r1cs_constraints();

		if( constrainOutputs )
		{
			unsigned i = 0;
			for( const auto &lc : last_round.outputs )
			{
				this->pb.add_r1cs_constraint(
					ConstraintT(lc, libsnark::ONE, _output_vars[i]),
					FMT(this->annotation_prefix, ".output[%u] = last_round.output[%u]", i, i));
				i += 1;
			}
		}
	}

	void generate_r1cs_witness() const
	{
		first_round.generate_r1cs_witness();

		for( auto& prefix_round : prefix_full_rounds ) {
			prefix_round.generate_r1cs_witness();
		}

		for( auto& partial_round : partial_rounds ) {
			partial_round.generate_r1cs_witness();
		}

		for( auto& suffix_round : suffix_full_rounds ) {
			suffix_round.generate_r1cs_witness();
		}

		last_round.generate_r1cs_witness();

		// When outputs are constrained, fill in the variable
		if( constrainOutputs )
		{
			for (unsigned int n = 0; n < last_round.outputs.size(); n++)
			{
				pb.val(_output_vars[n]) = lc_val(pb, last_round.outputs[n]);
			}
		}
	}
};


template<unsigned param_t, unsigned param_c, unsigned param_F, unsigned param_P, unsigned nInputs, unsigned nOutputs, bool constrainOutputs=true>
class Poseidon_gadget_T : public GadgetT, public libsnark::ITranslator
{
public:
	typedef Master_Poseidon_gadget_T<param_t, param_c, param_F, param_P, nInputs, nOutputs, constrainOutputs> Master;
	const Master& master;

	VariableArrayT instance_inputs;
	unsigned int instance_variables_offset;
	VariableT res;

	const Master& get_master()
	{
		static ProtoboardT master_pb;
		static Master* master;
		static std::once_flag flag;
		std::call_once(flag, [](){
			master = new Master(master_pb, make_var_array(master_pb, nInputs, ".dummy_inputs"), ".poseidon_master");
			master->generate_r1cs_constraints();
			master_pb.set_use_thread_values(true);
		});
		return *master;
	}

	Poseidon_gadget_T(
		ProtoboardT &pb,
		const VariableArrayT& in_inputs,
		const std::string& annotation_prefix
	) :
		GadgetT(pb, annotation_prefix),
		master(get_master()),
		instance_inputs(in_inputs)
	{
		// Keep track of where the variable for this instance start
		instance_variables_offset = pb.num_variables() + 1;
		// Allocate the variables on the pb needed for this instance
		make_var_array(pb, master.pb.num_variables() - in_inputs.size(), FMT(annotation_prefix, ".instance_var"));
		// We need to return a reference to the output variable so create the variable here
		res = VariableT(translate(master._output_vars[0].index));
	}

	~Poseidon_gadget_T()
	{
		//std::cout << "destructor" << std::endl;
	}

	void generate_r1cs_constraints() const
	{
		// For now, still copy all constraints to the main pb
		const auto& constraints = master.pb.constraint_system.constraints;
		for(unsigned int i = 0; i < constraints.size(); i++)
		{
			pb.constraint_system.constraints.emplace_back(
				libsnark::make_unique<libsnark::r1cs_constraint_light_instance<FieldT>>(
					(libsnark::r1cs_constraint_light<FieldT>*) constraints[i].get(),
					(libsnark::ITranslator*) this
				)
			);
		}
	}

	void generate_r1cs_witness() const
	{
		// TODO: this can be done smarter by replacing the variable indices in the background
		// Set the input values
		for (unsigned int i = 0; i < instance_inputs.size(); i++)
		{
			master.pb.val(1 + i) = pb.val(instance_inputs[i]);
		}
		// Calculate the funtion witnesses
		master.generate_r1cs_witness();
		// Copy variable values
		for (unsigned int i = 0; i < master.pb.num_variables() - instance_inputs.size(); i++)
		{
			pb.val(instance_variables_offset + i) = master.pb.val(1 + instance_inputs.size() + i);
		}
	}

	template<bool x = constrainOutputs, unsigned n = nOutputs>
	typename std::enable_if<x && n == 1, const VariableT&>::type
	result() const
	{
		return res;
	}

	unsigned int translate(unsigned int index) const override
	{
		if (index == 0)
		{
			return 0;
		}
		else if (index <= instance_inputs.size())
		{
			return instance_inputs[index - 1].index;
		}
		else
		{
			return instance_variables_offset + (index - (1 + instance_inputs.size()));
		}
	}

	void swapAB() override
	{
		static std::once_flag flag;
		std::call_once(flag, [&](){
			const auto& constraints = master.pb.constraint_system.constraints;
			for(unsigned int i = 0; i < constraints.size(); i++)
			{
				constraints[i]->swapAB();
			}
		});
	}
};


template<unsigned nInputs, unsigned nOutputs, bool constrainOutputs=true>
using Poseidon128 = Poseidon_gadget_T<6, 1, 8, 57, nInputs, nOutputs, constrainOutputs>;


// namespace ethsnarks
}

#endif
